#pragma once

#include "routing/base/astar_weight.hpp"

#include "base/assert.hpp"
#include "base/cancellable.hpp"

#include <algorithm>
#include <iostream>
#include <map>
#include <queue>
#include <vector>

namespace routing
{

template <typename TVertexType, typename TWeightType>
struct RoutingResult
{
  std::vector<TVertexType> path;
  TWeightType distance;

  RoutingResult() : distance(GetAStarWeightZero<TWeightType>()) {}

  void Clear()
  {
    path.clear();
    distance = GetAStarWeightZero<TWeightType>();
  }
};

template <typename TGraph>
class AStarAlgorithm
{
public:
  using TGraphType = TGraph;
  using TVertexType = typename TGraphType::TVertexType;
  using TEdgeType = typename TGraphType::TEdgeType;
  using TWeightType = typename TGraphType::TWeightType;

  enum class Result
  {
    OK,
    NoPath,
    Cancelled
  };

  friend std::ostream & operator<<(std::ostream & os, Result const & result)
  {
    switch (result)
    {
      case Result::OK:
        os << "OK";
        break;
      case Result::NoPath:
        os << "NoPath";
        break;
      case Result::Cancelled:
        os << "Cancelled";
        break;
    }
    return os;
  }

  using TOnVisitedVertexCallback = std::function<void(TVertexType const &, TVertexType const &)>;

  class Context final
  {
  public:
    void Clear()
    {
      m_distanceMap.clear();
      m_parents.clear();
    }

    bool HasDistance(TVertexType const & vertex) const
    {
      return m_distanceMap.find(vertex) != m_distanceMap.cend();
    }

    TWeightType GetDistance(TVertexType const & vertex) const
    {
      auto const & it = m_distanceMap.find(vertex);
      if (it == m_distanceMap.cend())
        return kInfiniteDistance;

      return it->second;
    }

    void SetDistance(TVertexType const & vertex, TWeightType const & distance)
    {
      m_distanceMap[vertex] = distance;
    }

    void SetParent(TVertexType const & parent, TVertexType const & child)
    {
      m_parents[parent] = child;
    }

    void ReconstructPath(TVertexType const & v, std::vector<TVertexType> & path) const;

  private:
    std::map<TVertexType, TWeightType> m_distanceMap;
    std::map<TVertexType, TVertexType> m_parents;
  };

  // VisitVertex returns true: wave will continue
  // VisitVertex returns false: wave will stop
  template <typename VisitVertex, typename AdjustEdgeWeight>
  void PropagateWave(TGraphType & graph, TVertexType const & startVertex,
                     VisitVertex && visitVertex, AdjustEdgeWeight && adjustEdgeWeight,
                     Context & context) const;

  template <typename VisitVertex>
  void PropagateWave(TGraphType & graph, TVertexType const & startVertex,
                     VisitVertex && visitVertex, Context & context) const;

  Result FindPath(TGraphType & graph, TVertexType const & startVertex,
                  TVertexType const & finalVertex, RoutingResult<TVertexType, TWeightType> & result,
                  my::Cancellable const & cancellable = my::Cancellable(),
                  TOnVisitedVertexCallback onVisitedVertexCallback = nullptr) const;

  Result FindPathBidirectional(TGraphType & graph,
                               TVertexType const & startVertex, TVertexType const & finalVertex,
                               RoutingResult<TVertexType, TWeightType> & result,
                               my::Cancellable const & cancellable = my::Cancellable(),
                               TOnVisitedVertexCallback onVisitedVertexCallback = nullptr) const;

  // Adjust route to the previous one.
  // adjustLimit - distance limit for wave propagation, measured in same units as graph edges length.
  typename AStarAlgorithm<TGraph>::Result AdjustRoute(
      TGraphType & graph, TVertexType const & startVertex, std::vector<TEdgeType> const & prevRoute,
      TWeightType const & adjustLimit, RoutingResult<TVertexType, TWeightType> & result,
      my::Cancellable const & cancellable, TOnVisitedVertexCallback onVisitedVertexCallback) const;

private:
  // Periodicity of switching a wave of bidirectional algorithm.
  static uint32_t constexpr kQueueSwitchPeriod = 128;

  // Precision of comparison weights.
  static TWeightType constexpr kEpsilon = GetAStarWeightEpsilon<TWeightType>();
  static TWeightType constexpr kZeroDistance = GetAStarWeightZero<TWeightType>();
  static TWeightType constexpr kInfiniteDistance = GetAStarWeightMax<TWeightType>();

  class PeriodicPollCancellable final
  {
  public:
    PeriodicPollCancellable(my::Cancellable const & cancellable) : m_cancellable(cancellable) {}

    bool IsCancelled()
    {
      // Periodicity of checking is cancellable cancelled.
      uint32_t constexpr kPeriod = 128;
      return count++ % kPeriod == 0 && m_cancellable.IsCancelled();
    }

  private:
    my::Cancellable const & m_cancellable;
    uint32_t count = 0;
  };

  // State is what is going to be put in the priority queue. See the
  // comment for FindPath for more information.
  struct State
  {
    State(TVertexType const & vertex, TWeightType const & distance)
      : vertex(vertex), distance(distance)
    {
    }

    inline bool operator>(State const & rhs) const { return distance > rhs.distance; }

    TVertexType vertex;
    TWeightType distance;
  };

  // BidirectionalStepContext keeps all the information that is needed to
  // search starting from one of the two directions. Its main
  // purpose is to make the code that changes directions more readable.
  struct BidirectionalStepContext
  {
    BidirectionalStepContext(bool forward, TVertexType const & startVertex,
                             TVertexType const & finalVertex, TGraphType & graph)
        : forward(forward), startVertex(startVertex), finalVertex(finalVertex), graph(graph)
        , m_piRT(graph.HeuristicCostEstimate(finalVertex, startVertex))
        , m_piFS(graph.HeuristicCostEstimate(startVertex, finalVertex))
    {
      bestVertex = forward ? startVertex : finalVertex;
      pS = ConsistentHeuristic(bestVertex);
    }

    TWeightType TopDistance() const
    {
      ASSERT(!queue.empty(), ());
      return bestDistance.at(queue.top().vertex);
    }

    // p_f(v) = 0.5*(π_f(v) - π_r(v)) + 0.5*π_r(t)
    // p_r(v) = 0.5*(π_r(v) - π_f(v)) + 0.5*π_f(s)
    // p_r(v) + p_f(v) = const. Note: this condition is called consistence.
    TWeightType ConsistentHeuristic(TVertexType const & v) const
    {
      auto const piF = graph.HeuristicCostEstimate(v, finalVertex);
      auto const piR = graph.HeuristicCostEstimate(v, startVertex);
      if (forward)
      {
        /// @todo careful: with this "return" here and below in the Backward case
        /// the heuristic becomes inconsistent but still seems to work.
        /// return HeuristicCostEstimate(v, finalVertex);
        return 0.5 * (piF - piR + m_piRT);
      }
      else
      {
        // return HeuristicCostEstimate(v, startVertex);
        return 0.5 * (piR - piF + m_piFS);
      }
    }

    void GetAdjacencyList(TVertexType const & v, std::vector<TEdgeType> & adj)
    {
      if (forward)
        graph.GetOutgoingEdgesList(v, adj);
      else
        graph.GetIngoingEdgesList(v, adj);
    }

    bool const forward;
    TVertexType const & startVertex;
    TVertexType const & finalVertex;
    TGraph & graph;
    TWeightType const m_piRT;
    TWeightType const m_piFS;

    std::priority_queue<State, std::vector<State>, std::greater<State>> queue;
    std::map<TVertexType, TWeightType> bestDistance;
    std::map<TVertexType, TVertexType> parent;
    TVertexType bestVertex;

    TWeightType pS;
  };

  static void ReconstructPath(TVertexType const & v,
                              std::map<TVertexType, TVertexType> const & parent,
                              std::vector<TVertexType> & path);
  static void ReconstructPathBidirectional(TVertexType const & v, TVertexType const & w,
                                           std::map<TVertexType, TVertexType> const & parentV,
                                           std::map<TVertexType, TVertexType> const & parentW,
                                           std::vector<TVertexType> & path);
};

template <typename TGraph>
constexpr typename TGraph::TWeightType AStarAlgorithm<TGraph>::kEpsilon;
template <typename TGraph>
constexpr typename TGraph::TWeightType AStarAlgorithm<TGraph>::kInfiniteDistance;
template <typename TGraph>
constexpr typename TGraph::TWeightType AStarAlgorithm<TGraph>::kZeroDistance;

template <typename TGraph>
template <typename VisitVertex, typename AdjustEdgeWeight>
void AStarAlgorithm<TGraph>::PropagateWave(TGraphType & graph, TVertexType const & startVertex,
                                           VisitVertex && visitVertex,
                                           AdjustEdgeWeight && adjustEdgeWeight,
                                           AStarAlgorithm<TGraph>::Context & context) const
{
  context.Clear();

  std::priority_queue<State, std::vector<State>, std::greater<State>> queue;

  context.SetDistance(startVertex, kZeroDistance);
  queue.push(State(startVertex, kZeroDistance));

  std::vector<TEdgeType> adj;

  while (!queue.empty())
  {
    State const stateV = queue.top();
    queue.pop();

    if (stateV.distance > context.GetDistance(stateV.vertex))
      continue;

    if (!visitVertex(stateV.vertex))
      return;

    graph.GetOutgoingEdgesList(stateV.vertex, adj);
    for (auto const & edge : adj)
    {
      State stateW(edge.GetTarget(), kZeroDistance);
      if (stateV.vertex == stateW.vertex)
        continue;

      auto const edgeWeight = adjustEdgeWeight(stateV.vertex, edge);
      auto const newReducedDist = stateV.distance + edgeWeight;

      if (newReducedDist >= context.GetDistance(stateW.vertex) - kEpsilon)
        continue;

      stateW.distance = newReducedDist;
      context.SetDistance(stateW.vertex, newReducedDist);
      context.SetParent(stateW.vertex, stateV.vertex);
      queue.push(stateW);
    }
  }
}

template <typename TGraph>
template <typename VisitVertex>
void AStarAlgorithm<TGraph>::PropagateWave(TGraphType & graph, TVertexType const & startVertex,
                                           VisitVertex && visitVertex,
                                           AStarAlgorithm<TGraph>::Context & context) const
{
  auto const adjustEdgeWeight = [](TVertexType const & /* vertex */, TEdgeType const & edge) {
    return edge.GetWeight();
  };
  PropagateWave(graph, startVertex, visitVertex, adjustEdgeWeight, context);
}

// This implementation is based on the view that the A* algorithm
// is equivalent to Dijkstra's algorithm that is run on a reweighted
// version of the graph. If an edge (v, w) has length l(v, w), its reduced
// cost is l_r(v, w) = l(v, w) + pi(w) - pi(v), where pi() is any function
// that ensures l_r(v, w) >= 0 for every edge. We set pi() to calculate
// the shortest possible distance to a goal node, and this is a common
// heuristic that people use in A*.
// Refer to these papers for more information:
// http://research.microsoft.com/pubs/154937/soda05.pdf
// http://www.cs.princeton.edu/courses/archive/spr06/cos423/Handouts/EPP%20shortest%20path%20algorithms.pdf

template <typename TGraph>
typename AStarAlgorithm<TGraph>::Result AStarAlgorithm<TGraph>::FindPath(
    TGraphType & graph, TVertexType const & startVertex, TVertexType const & finalVertex,
    RoutingResult<TVertexType, TWeightType> & result, my::Cancellable const & cancellable,
    TOnVisitedVertexCallback onVisitedVertexCallback) const
{
  result.Clear();
  if (nullptr == onVisitedVertexCallback)
    onVisitedVertexCallback = [](TVertexType const &, TVertexType const &) {};

  Context context;
  PeriodicPollCancellable periodicCancellable(cancellable);
  Result resultCode = Result::NoPath;

  auto visitVertex = [&](TVertexType const & vertex) {
    if (periodicCancellable.IsCancelled())
    {
      resultCode = Result::Cancelled;
      return false;
    }

    onVisitedVertexCallback(vertex, finalVertex);

    if (vertex == finalVertex)
    {
      resultCode = Result::OK;
      return false;
    }

    return true;
  };

  auto adjustEdgeWeight = [&](TVertexType const & vertex, TEdgeType const & edge) {
    auto const len = edge.GetWeight();
    auto const piV = graph.HeuristicCostEstimate(vertex, finalVertex);
    auto const piW = graph.HeuristicCostEstimate(edge.GetTarget(), finalVertex);
    auto const reducedLen = len + piW - piV;

    CHECK(reducedLen >= -kEpsilon, ("Invariant violated:", reducedLen, "<", -kEpsilon));
    return std::max(reducedLen, kZeroDistance);
  };

  PropagateWave(graph, startVertex, visitVertex, adjustEdgeWeight, context);

  if (resultCode == Result::OK)
  {
    context.ReconstructPath(finalVertex, result.path);
    result.distance =
        context.GetDistance(finalVertex) - graph.HeuristicCostEstimate(startVertex, finalVertex);
  }

  return resultCode;
}

template <typename TGraph>
typename AStarAlgorithm<TGraph>::Result AStarAlgorithm<TGraph>::FindPathBidirectional(
    TGraphType & graph,
    TVertexType const & startVertex, TVertexType const & finalVertex,
    RoutingResult<TVertexType, TWeightType> & result,
    my::Cancellable const & cancellable,
    TOnVisitedVertexCallback onVisitedVertexCallback) const
{
  if (nullptr == onVisitedVertexCallback)
    onVisitedVertexCallback = [](TVertexType const &, TVertexType const &){};

  BidirectionalStepContext forward(true /* forward */, startVertex, finalVertex, graph);
  BidirectionalStepContext backward(false /* forward */, startVertex, finalVertex, graph);

  bool foundAnyPath = false;
  auto bestPathReducedLength = kZeroDistance;
  auto bestPathRealLength = kZeroDistance;

  forward.bestDistance[startVertex] = kZeroDistance;
  forward.queue.push(State(startVertex, kZeroDistance));

  backward.bestDistance[finalVertex] = kZeroDistance;
  backward.queue.push(State(finalVertex, kZeroDistance));

  // To use the search code both for backward and forward directions
  // we keep the pointers to everything related to the search in the
  // 'current' and 'next' directions. Swapping these pointers indicates
  // changing the end we are searching from.
  BidirectionalStepContext * cur = &forward;
  BidirectionalStepContext * nxt = &backward;

  std::vector<TEdgeType> adj;

  // It is not necessary to check emptiness for both queues here
  // because if we have not found a path by the time one of the
  // queues is exhausted, we never will.
  uint32_t steps = 0;
  PeriodicPollCancellable periodicCancellable(cancellable);

  while (!cur->queue.empty() && !nxt->queue.empty())
  {
    ++steps;

    if (periodicCancellable.IsCancelled())
      return Result::Cancelled;

    if (steps % kQueueSwitchPeriod == 0)
      std::swap(cur, nxt);

    if (foundAnyPath)
    {
      auto const curTop = cur->TopDistance();
      auto const nxtTop = nxt->TopDistance();

      // The intuition behind this is that we cannot obtain a path shorter
      // than the left side of the inequality because that is how any path we find
      // will look like (see comment for curPathReducedLength below).
      // We do not yet have the proof that we will not miss a good path by doing so.

      // The shortest reduced path corresponds to the shortest real path
      // because the heuristics we use are consistent.
      // It would be a mistake to make a decision based on real path lengths because
      // several top states in a priority queue may have equal reduced path lengths and
      // different real path lengths.

      if (curTop + nxtTop >= bestPathReducedLength - kEpsilon)
      {
        ReconstructPathBidirectional(cur->bestVertex, nxt->bestVertex, cur->parent, nxt->parent,
                                     result.path);
        result.distance = bestPathRealLength;
        CHECK(!result.path.empty(), ());
        if (!cur->forward)
          reverse(result.path.begin(), result.path.end());
        return Result::OK;
      }
    }

    State const stateV = cur->queue.top();
    cur->queue.pop();

    if (stateV.distance > cur->bestDistance[stateV.vertex])
      continue;

    onVisitedVertexCallback(stateV.vertex, cur->forward ? cur->finalVertex : cur->startVertex);

    cur->GetAdjacencyList(stateV.vertex, adj);
    for (auto const & edge : adj)
    {
      State stateW(edge.GetTarget(), kZeroDistance);
      if (stateV.vertex == stateW.vertex)
        continue;

      auto const len = edge.GetWeight();
      auto const pV = cur->ConsistentHeuristic(stateV.vertex);
      auto const pW = cur->ConsistentHeuristic(stateW.vertex);
      auto const reducedLen = len + pW - pV;

      CHECK(reducedLen >= -kEpsilon, ("Invariant violated:", reducedLen, "<", -kEpsilon));
      auto const newReducedDist = stateV.distance + std::max(reducedLen, kZeroDistance);

      auto const itCur = cur->bestDistance.find(stateW.vertex);
      if (itCur != cur->bestDistance.end() && newReducedDist >= itCur->second - kEpsilon)
        continue;

      auto const itNxt = nxt->bestDistance.find(stateW.vertex);
      if (itNxt != nxt->bestDistance.end())
      {
        auto const distW = itNxt->second;
        // Reduced length that the path we've just found has in the original graph:
        // find the reduced length of the path's parts in the reduced forward and backward graphs.
        auto const curPathReducedLength = newReducedDist + distW;
        // No epsilon here: it is ok to overshoot slightly.
        if (!foundAnyPath || bestPathReducedLength > curPathReducedLength)
        {
          bestPathReducedLength = curPathReducedLength;

          bestPathRealLength = stateV.distance + len + distW;
          bestPathRealLength += cur->pS - pV;
          bestPathRealLength += nxt->pS - nxt->ConsistentHeuristic(stateW.vertex);

          foundAnyPath = true;
          cur->bestVertex = stateV.vertex;
          nxt->bestVertex = stateW.vertex;
        }
      }

      stateW.distance = newReducedDist;
      cur->bestDistance[stateW.vertex] = newReducedDist;
      cur->parent[stateW.vertex] = stateV.vertex;
      cur->queue.push(stateW);
    }
  }

  return Result::NoPath;
}

template <typename TGraph>
typename AStarAlgorithm<TGraph>::Result AStarAlgorithm<TGraph>::AdjustRoute(
    TGraphType & graph, TVertexType const & startVertex, std::vector<TEdgeType> const & prevRoute,
    TWeightType const & adjustLimit, RoutingResult<TVertexType, TWeightType> & result,
    my::Cancellable const & cancellable, TOnVisitedVertexCallback onVisitedVertexCallback) const
{
  CHECK(!prevRoute.empty(), ());

  result.Clear();
  if (onVisitedVertexCallback == nullptr)
    onVisitedVertexCallback = [](TVertexType const &, TVertexType const &) {};

  bool wasCancelled = false;
  auto minDistance = kInfiniteDistance;
  TVertexType returnVertex;

  std::map<TVertexType, TWeightType> remainingDistances;
  auto remainingDistance = kZeroDistance;

  for (auto it = prevRoute.crbegin(); it != prevRoute.crend(); ++it)
  {
    remainingDistances[it->GetTarget()] = remainingDistance;
    remainingDistance += it->GetWeight();
  }

  Context context;
  PeriodicPollCancellable periodicCancellable(cancellable);

  auto visitVertex = [&](TVertexType const & vertex) {

    if (periodicCancellable.IsCancelled())
    {
      wasCancelled = true;
      return false;
    }

    auto const distance = context.GetDistance(vertex);
    if (distance > adjustLimit)
      return false;

    onVisitedVertexCallback(startVertex, vertex);

    auto it = remainingDistances.find(vertex);
    if (it != remainingDistances.cend())
    {
      auto const fullDistance = distance + it->second;
      if (fullDistance < minDistance)
      {
        minDistance = fullDistance;
        returnVertex = vertex;
      }
    }

    return true;
  };

  PropagateWave(graph, startVertex, visitVertex, context);
  if (wasCancelled)
    return Result::Cancelled;

  if (minDistance == kInfiniteDistance)
    return Result::NoPath;

  context.ReconstructPath(returnVertex, result.path);

  // Append remaining route.
  bool found = false;
  for (size_t i = 0; i < prevRoute.size(); ++i)
  {
    if (prevRoute[i].GetTarget() == returnVertex)
    {
      for (size_t j = i + 1; j < prevRoute.size(); ++j)
        result.path.push_back(prevRoute[j].GetTarget());

      found = true;
      break;
    }
  }

  CHECK(found,
        ("Can't find", returnVertex, ", prev:", prevRoute.size(), ", adjust:", result.path.size()));

  auto const & it = remainingDistances.find(returnVertex);
  CHECK(it != remainingDistances.end(), ());
  result.distance = context.GetDistance(returnVertex) + it->second;
  return Result::OK;
}

// static
template <typename TGraph>
void AStarAlgorithm<TGraph>::ReconstructPath(TVertexType const & v,
                                             std::map<TVertexType, TVertexType> const & parent,
                                             std::vector<TVertexType> & path)
{
  path.clear();
  TVertexType cur = v;
  while (true)
  {
    path.push_back(cur);
    auto it = parent.find(cur);
    if (it == parent.end())
      break;
    cur = it->second;
  }
  reverse(path.begin(), path.end());
}

// static
template <typename TGraph>
void AStarAlgorithm<TGraph>::ReconstructPathBidirectional(
    TVertexType const & v, TVertexType const & w,
    std::map<TVertexType, TVertexType> const & parentV,
    std::map<TVertexType, TVertexType> const & parentW, std::vector<TVertexType> & path)
{
  std::vector<TVertexType> pathV;
  ReconstructPath(v, parentV, pathV);
  std::vector<TVertexType> pathW;
  ReconstructPath(w, parentW, pathW);
  path.clear();
  path.reserve(pathV.size() + pathW.size());
  path.insert(path.end(), pathV.begin(), pathV.end());
  path.insert(path.end(), pathW.rbegin(), pathW.rend());
}

template <typename TGraph>
void AStarAlgorithm<TGraph>::Context::ReconstructPath(TVertexType const & v,
                                                      std::vector<TVertexType> & path) const
{
  AStarAlgorithm<TGraph>::ReconstructPath(v, m_parents, path);
}
}  // namespace routing
