/**
 * @file PathPlanner.h
 * @author Tobias Kunz (?), Can Erdogan, Michael X. Grey
 * @date Jan 30, 2013
 * @brief Contains the path planner class definition which is templated on a RRT implementation and
 * creates an interface to generate trajectories with different RRT algorithms such as goal-biased,
 * bidirectional, connect and etc.
 */

#ifndef DART_PLANNING_PATHPLANNER_H_
#define DART_PLANNING_PATHPLANNER_H_

#include <Eigen/Core>
#include <iostream>
#include <limits>
#include <list>
#include <vector>
#include "dart/dynamics/Skeleton.h"
#include "dart/simulation/World.h"
#include "dart/planning/RRT.h"
#include <cstdio>

namespace dart {
namespace planning {

/* ********************************************************************************************* */
/// The path planner class - a common interface to motion planners
template <class R = RRT>
class PathPlanner {
public:

  bool connect;            ///< Whether we take a small step or shoot for the target node
  bool bidirectional;      ///< Whether two trees try to meet each other or one goes for the goal
  double stepSize;        ///< Step size from a node in the tree to the random/goal node
  double goalBias;        ///< Choose btw goal and random value (for goal-biased search)
  size_t maxNodes;        ///< Maximum number of iterations the sampling would continue
  simulation::WorldPtr world;  ///< The world that the robot is in (for obstacles and etc.)
  std::shared_ptr<optimizer::Solver> solver; ///< A problem which should be solved by each configuration

  // NOTE: It is useful to keep the rrts around after planning for reuse, analysis, and etc.
  std::unique_ptr<R> start_rrt;            ///< The rrt for unidirectional search
  std::unique_ptr<R> goal_rrt;              ///< The second rrt if bidirectional search is executed

public:

  /// The default constructor
  PathPlanner() : world(nullptr), mRD(), mMT(mRD()), mDistribution(0.0, std::nextafter(1.0, 2.0)) {}

  /// The desired constructor - you should use this one.
  PathPlanner(simulation::World& world, bool bidirectional_ = true, bool connect_ = true, double stepSize_ = 0.1,
    size_t maxNodes_ = 1e6, double goalBias_ = 0.3) : world(&world), bidirectional(bidirectional_),
    connect(connect_), stepSize(stepSize_), maxNodes(maxNodes_), goalBias(goalBias_),
    mRD(), mMT(mRD()), mDistribution(0.0, std::nextafter(1.0, 2.0))
  {
    // Do nothing
  }

  /// The destructor
  virtual ~PathPlanner() {}

  /// Plan a path from a single start configuration to a single goal
  bool planPath(const dynamics::SkeletonPtr& robot,
                const std::vector<size_t>& dofs, const Eigen::VectorXd& start,
                const Eigen::VectorXd& goal, std::list<Eigen::VectorXd>& path)
  {
    std::vector<Eigen::VectorXd> startVector, goalVector;
    startVector.push_back(start);
    goalVector.push_back(goal);
    return planPath(robot, dofs, startVector, goalVector, path);
  }

  /// Plan a path from a _set_ of start configurations to a _set_ of goals
  bool planPath(const dynamics::SkeletonPtr& robot,
                const std::vector<size_t>& dofs,
                const std::vector<Eigen::VectorXd>& start,
                const std::vector<Eigen::VectorXd>& goal,
                std::list<Eigen::VectorXd>& path);

private:

  /// Performs a unidirectional RRT with the given options.
  bool planSingleTreeRrt(const dynamics::SkeletonPtr& robot,
                         const std::vector<size_t>& dofs,
                         const std::vector<Eigen::VectorXd>& start,
                         const Eigen::VectorXd& goal,
                         std::list<Eigen::VectorXd>& path);

  /// Performs bidirectional RRT with the given options.
  /// NOTE This algorithm has several different popular implementations. The implementation in the
  /// kinodynamic paper (1999) by LaValle and Kuffner extend the two RRTs towards a common random
  /// configurations whereas here, first, start rrt extends towards a random node and creates
  /// some node N. Afterwards, the second rrt extends towards _the node N_ and they continue
  /// swapping roles.
  bool planBidirectionalRrt(const dynamics::SkeletonPtr& robot,
                            const std::vector<size_t>& dofs,
                            const std::vector<Eigen::VectorXd>& start,
                            const std::vector<Eigen::VectorXd>& goal,
                            std::list<Eigen::VectorXd>& path);

  /// Randomization device
  std::random_device mRD;

  /// Mersene twister method
  mutable std::mt19937 mMT;

  /// Distribution
  mutable std::uniform_real_distribution<double> mDistribution;
};

/* ********************************************************************************************* */
template <class R>
bool PathPlanner<R>::planPath(const dynamics::SkeletonPtr& robot,
                              const std::vector<size_t>& dofs,
                              const std::vector<Eigen::VectorXd>& start,
                              const std::vector<Eigen::VectorXd>& goal,
                              std::list<Eigen::VectorXd>& path)
{
  Eigen::VectorXd savedConfiguration = robot->getPositions(dofs);

  // ====================================================================
  // Check for collisions in the start and goal configurations

  // Sift through the possible start configurations and eliminate those that are in collision
  std::vector<Eigen::VectorXd> feasibleStart;
  for(unsigned int i = 0; i < start.size(); i++)
  {
    robot->setPositions(dofs, start[i]);
    if(!world->checkCollision()) feasibleStart.push_back(start[i]);
  }

  // Return false if there are no feasible start configurations
  if(feasibleStart.empty())
  {
    dtwarn << "WARNING: PathPlanner: Feasible start points are empty!\n";
    return false;
  }

  // Sift through the possible goal configurations and eliminate those that are in collision
  std::vector<Eigen::VectorXd> feasibleGoal;
  for(unsigned int i = 0; i < goal.size(); i++)
  {
    robot->setPositions(dofs, goal[i]);
    if(!world->checkCollision()) feasibleGoal.push_back(goal[i]);
  }

  // Return false if there are no feasible goal configurations
  if(feasibleGoal.empty())
  {
    dtwarn << "WARNING: PathPlanner: Feasible goal points are empty!\n";
    return false;
  }

  // ====================================================================
  // Make the correct RRT algorithm for the given method

  // Direct the search towards single or bidirectional
  bool result = false;
  if(bidirectional)
    result = planBidirectionalRrt(robot, dofs, feasibleStart, feasibleGoal, path);
  else
  {
    if(feasibleGoal.size() > 1)
      dtwarn << "WARNING: planPath is using ONLY the first goal!\n";

    result = planSingleTreeRrt(robot, dofs, feasibleStart, feasibleGoal.front(), path);
  }

  // Restore previous robot configuration
  robot->setPositions(dofs, savedConfiguration);

  return result;
}

/* ********************************************************************************************* */
template <class R>
bool PathPlanner<R>::planSingleTreeRrt(
    const dynamics::SkeletonPtr& robot, const std::vector<size_t>& dofs,
    const std::vector<Eigen::VectorXd>& start, const Eigen::VectorXd& goal,
    std::list<Eigen::VectorXd>& path)
{
  const bool debug = false;

  // Initialize the RRT
  if(nullptr == start_rrt)
    start_rrt = std::unique_ptr<R>(new R(world, robot, dofs, start, stepSize, solver));
  else
    start_rrt->reset(world, robot, dofs, start, stepSize, solver);

  // Expand the tree until the goal is reached or the max # nodes is passed
  size_t numNodes = start_rrt->getSize();
  while(numNodes <= maxNodes)
  {
    // Get the target node based on the bias
    Eigen::VectorXd target;
    double randomValue = mDistribution(mMT);
    if(randomValue < goalBias)
      target = goal;
    else
      target = start_rrt->getRandomConfig();

    // Based on the method, either attempt to connect to the target directly or take a small step
    if(connect)
      start_rrt->connect(target);
    else
      start_rrt->tryStep(target);

    // Check if the goal is reached and create the path, if so
    double gap = start_rrt->getGap(goal);
    if(gap < stepSize)
    {
      if(debug) std::cout << "Returning true, reached the goal" << std::endl;
      start_rrt->tracePath(start_rrt->mActiveNode, path);
      return true;
    }

    // Update the number of nodes
    numNodes = start_rrt->getSize();
  }

  if(debug)
    printf("numNodes: %lu\n", numNodes);

  // Maximum # of iterations are reached and path is not found - failed.
  return false;
}

/* ********************************************************************************************* */
template <class R>
bool PathPlanner<R>::planBidirectionalRrt(
    const dynamics::SkeletonPtr& robot, const std::vector<size_t>& dofs,
    const std::vector<Eigen::VectorXd>& start,
    const std::vector<Eigen::VectorXd>& goal,
    std::list<Eigen::VectorXd>& path) {

  const bool debug = false;

  // Initialize both the start and goal RRTs.
  // NOTE: We use the pointers for the RRTs to swap their roles in extending towards a target
  // (random or goal) node.

  if(nullptr == start_rrt)
    start_rrt = std::unique_ptr<R>(new R(world, robot, dofs, start, stepSize, solver));
  else
    start_rrt->reset(world, robot, dofs, start, stepSize, solver);

  if(nullptr == goal_rrt)
    goal_rrt = std::unique_ptr<R>(new R(world, robot, dofs, goal, stepSize, solver));
  else
    goal_rrt->reset(world, robot, dofs, goal, stepSize, solver);

  R* rrt1 = start_rrt.get();
  R* rrt2 = goal_rrt.get();

  // Expand the tree until the trees meet or the max # nodes is passed
  double smallestGap = std::numeric_limits<double>::infinity();
  size_t numNodes = rrt1->getSize() + rrt2->getSize();
  while(numNodes < maxNodes) {

    // Swap the roles of the two RRTs. Remember, the first rrt reaches out to a target node and
    // creates a new node and the second rrt reaches to _the new node_.
    std::swap(rrt1, rrt2);

     // Get the target node based on the bias
    Eigen::VectorXd target;
    double randomValue = mDistribution(mMT);
    if(randomValue < goalBias)
      target = goal[0];
    else
      target = rrt1->getRandomConfig();

    // Based on the method, rrt1 either attempt to connect to the target directly or takes a step
    if(connect)
      rrt1->connect(target);
    else
      rrt1->tryStep(target);

    // rrt2 uses the last added node of rrt1 as a target and reaches out to it (connect or step)
    // NOTE: If a node was not added, the nearest neighbor to the random node in rrt1 is used.
    // NOTE: connect(x) and tryStep(x) functions return true if rrt2 can add the given node
    // in the tree. In this case, this would imply that the two trees meet.
    bool treesMet = false;
    const Eigen::VectorXd& rrt2target = *(rrt1->mConfigs[rrt1->mActiveNode]);
    if(connect)
      treesMet = rrt2->connect(rrt2target);
    else
      treesMet = (rrt2->tryStep(rrt2target) == R::STEP_REACHED);

    // Check if the trees have met and create the path, if so.
    if(treesMet)
    {
      start_rrt->tracePath(start_rrt->mActiveNode, path);
      goal_rrt->tracePath(goal_rrt->mActiveNode, path, true);
      return true;
    }

    // Update the number of nodes in the two trees
    numNodes = rrt1->getSize() + rrt2->getSize();

    // Print the gap between the trees in debug mode
    if(debug)
    {
      double gap = rrt2->getGap(*(rrt1->mConfigs[rrt1->mActiveNode]));
      if(gap < smallestGap)
      {
        smallestGap = gap;
        std::cout << "Gap: " << smallestGap << "  Sizes: " << start_rrt->mConfigs.size()
          << "/" << goal_rrt->mConfigs.size() << std::endl;
      }
    }
  }

  // Maximum # of iterations are reached and path is not found - failed.
  return false;
}

} // namespace planning
} // namespace dart

#endif  // DART_PLANNING_PATHPLANNER_H_
