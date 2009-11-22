/***************************************************************************
  file : $URL$
  version : $LastChangedRevision$  $LastChangedBy$
  date : $LastChangedDate$
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 * Copyright (C) 2007 by Johan De Taeye                                    *
 *                                                                         *
 * This library is free software; you can redistribute it and/or modify it *
 * under the terms of the GNU Lesser General Public License as published   *
 * by the Free Software Foundation; either version 2.1 of the License, or  *
 * (at your option) any later version.                                     *
 *                                                                         *
 * This library is distributed in the hope that it will be useful,         *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of          *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser *
 * General Public License for more details.                                *
 *                                                                         *
 * You should have received a copy of the GNU Lesser General Public        *
 * License along with this library; if not, write to the Free Software     *
 * Foundation Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301,*
 * USA                                                                     *
 *                                                                         *
 ***************************************************************************/

#define FREPPLE_CORE
#include "frepple/model.h"
#include <climits>


// Uncomment the following line to create debugging statements during the
// clustering and leveling algorithm.
//#define CLUSTERDEBUG


namespace frepple
{


DECLARE_EXPORT bool HasLevel::recomputeLevels = false;
DECLARE_EXPORT bool HasLevel::computationBusy = false;
DECLARE_EXPORT short unsigned HasLevel::numberOfClusters = 0;
DECLARE_EXPORT short unsigned HasLevel::numberOfHangingClusters = 0;


DECLARE_EXPORT void HasLevel::computeLevels()
{
  computationBusy = true;
  // Get exclusive access to this function in a multi-threaded environment.
  static Mutex levelcomputationbusy;
  ScopeMutexLock l(levelcomputationbusy);

  // Another thread may already have computed the levels while this thread was
  // waiting for the lock. In that case the while loop will be skipped.
  while (recomputeLevels)
  {
    // Reset the recomputation flag. Note that during the computation the flag
    // could be switched on again by some model change in a different thread.
    // In that case, the while loop will be rerun.
    recomputeLevels = false;

    // Reset current levels on buffers, resources and operations
    for (Buffer::iterator gbuf = Buffer::begin();
        gbuf != Buffer::end(); ++gbuf)
    {
      gbuf->cluster = 0;
      gbuf->lvl = -1;
    }
    for (Resource::iterator gres = Resource::begin();
        gres != Resource::end(); ++gres)
    {
      gres->cluster = 0;
      gres->lvl = -1;
    }
    for (Operation::iterator gop = Operation::begin();
        gop != Operation::end(); ++gop)
    {
      gop->cluster = 0;
      gop->lvl = -1;
    }

    // Loop through all operations
    stack< pair<Operation*,int> > stack;
    Operation* cur_oper;
    int cur_level;
    Buffer *cur_buf;
    const Flow* cur_Flow;
    bool search_level;
    int cur_cluster;
    numberOfClusters = 0;
    numberOfHangingClusters = 0;
    set<Operation*> visited;
    for (Operation::iterator g = Operation::begin();
        g != Operation::end(); ++g)
    {

#ifdef CLUSTERDEBUG
      logger << "Investigating operation '" << &*g
      << "' - current cluster " << g->cluster << endl;
#endif

      // Select a new cluster number
      if (g->cluster)
        cur_cluster = g->cluster;
      else
      {
        cur_cluster = ++numberOfClusters;
        if (numberOfClusters >= USHRT_MAX)
          throw LogicException("Too many clusters");

        // Detect hanging operations
        if (g->getFlows().empty() && g->getLoads().empty() 
          && g->getSuperOperations().empty()
          && g->getSubOperations().empty()
          )
        {
          ++numberOfHangingClusters;
          g->lvl = 0;
          continue;
        }
      }

      // Do we need to activate the level search?
      // Criterion are:
      //   - Not used in a super operation
      //   - Have at no producing Flow on the Operation itself
      //     or on any of its sub operations
      search_level = false;
      if (g->getSuperOperations().empty())
      {
        search_level = true;
        // Does the operation itself have producing flows?
        for (Operation::flowlist::const_iterator fl = g->getFlows().begin();
            fl != g->getFlows().end() && search_level; ++fl)
          if (fl->isProducer()) search_level = false;
        if (search_level)
        {
          // Do suboperations have a producing flow?
          for (Operation::Operationlist::const_reverse_iterator
              i = g->getSubOperations().rbegin();
              i != g->getSubOperations().rend() && search_level;
              ++i)
            for (Operation::flowlist::const_iterator
                fl = (*i)->getFlows().begin();
                fl != (*i)->getFlows().end() && search_level;
                ++fl)
              if (fl->isProducer()) search_level = false;
        }
      }

      // If both the level and the cluster are de-activated, then we can move on
      if (!search_level && g->cluster) continue;

      // Start recursing
      // Note that as soon as push an operation on the stack we set its
      // cluster and/or level. This is avoid that operations are needlessly
      // pushed a second time on the stack.
      stack.push(make_pair(&*g, search_level ? 0 : -1));
      visited.clear();
      g->cluster = cur_cluster;
      if (search_level) g->lvl = 0;
      while (!stack.empty())
      {
        // Take the top of the stack
        cur_oper = stack.top().first;
        cur_level = stack.top().second;
        stack.pop();

#ifdef CLUSTERDEBUG
        logger << "    Recursing in Operation '" << *(cur_oper)
        << "' - current level " << cur_level << endl;
#endif
        // Detect loops in the supply chain
        if (visited.find(cur_oper) != visited.end())
          // Already visited this operation - don't repeat
          continue;
        else
          // Keep track of operations already visited
          visited.insert(cur_oper);

        // Push sub operations on the stack
        for (Operation::Operationlist::const_reverse_iterator
            i = cur_oper->getSubOperations().rbegin();
            i != cur_oper->getSubOperations().rend();
            ++i)
        {
          if ((*i)->lvl < cur_level)
          {
            // Search level and cluster
            stack.push(make_pair(*i,cur_level));
            (*i)->lvl = cur_level;
            (*i)->cluster = cur_cluster;
          }
          else if (!(*i)->cluster)
          {
            // Search for clusters information only
            stack.push(make_pair(*i,-1));
            (*i)->cluster = cur_cluster;
          }
          // else: no search required
        }

        // Push super operations on the stack
        for (Operation::Operationlist::const_reverse_iterator
            j = cur_oper->getSuperOperations().rbegin();
            j != cur_oper->getSuperOperations().rend();
            ++j)
        {
          if ((*j)->lvl < cur_level)
          {
            // Search level and cluster
            stack.push(make_pair(*j,cur_level));
            (*j)->lvl = cur_level;
            (*j)->cluster = cur_cluster;
          }
          else if (!(*j)->cluster)
          {
            // Search for clusters information only
            stack.push(make_pair(*j,-1));
            (*j)->cluster = cur_cluster;
          }
          // else: no search required
        }

        // Update level of resources linked to current operation
        for (Operation::loadlist::const_iterator gres =
              cur_oper->getLoads().begin();
            gres != cur_oper->getLoads().end(); ++gres)
        {
          Resource *resptr = gres->getResource();
          // Update the level of the resource
          if (resptr->lvl < cur_level) resptr->lvl = cur_level;
          // Update the cluster of the resource and operations using it
          if (!resptr->cluster)
          {
            resptr->cluster = cur_cluster;
            // Find more operations connected to this cluster by the Resource
            for (Resource::loadlist::const_iterator resops =
                  resptr->getLoads().begin();
                resops != resptr->getLoads().end(); ++resops)
              if (!resops->getOperation()->cluster)
              {
                stack.push(make_pair(resops->getOperation(),-1));
                resops->getOperation()->cluster = cur_cluster;
              }
          }
        }

        // Now loop through all flows of the operation
        for (Operation::flowlist::const_iterator
            gflow = cur_oper->getFlows().begin();
            gflow != cur_oper->getFlows().end();
            ++gflow)
        {
          cur_Flow = &*gflow;
          cur_buf = cur_Flow->getBuffer();

          // Check whether the level search needs to continue
          search_level = cur_level!=-1 && cur_buf->lvl<cur_level+1;

          // Check if the Buffer needs processing
          if (search_level || !cur_buf->cluster)
          {
            // Update the cluster of the current buffer
            cur_buf->cluster = cur_cluster;

            // Loop through all flows of the buffer
            for (Buffer::flowlist::const_iterator
                buffl = cur_buf->getFlows().begin();
                buffl != cur_buf->getFlows().end();
                ++buffl)
            {
              // Check level recursion
              if (cur_Flow->isConsumer() && search_level)
              {
                if (buffl->getOperation()->lvl < cur_level+1
                    && &*buffl != cur_Flow && buffl->isProducer())
                {
                  stack.push(make_pair(buffl->getOperation(),cur_level+1));
                  buffl->getOperation()->lvl = cur_level+1;
                  buffl->getOperation()->cluster = cur_cluster;
                }
                else if (!buffl->getOperation()->cluster)
                {
                  stack.push(make_pair(buffl->getOperation(),-1));
                  buffl->getOperation()->cluster = cur_cluster;
                }
                cur_buf->lvl = cur_level+1;
              }
              // Check cluster recursion
              else if (!buffl->getOperation()->cluster)
              {
                stack.push(make_pair(buffl->getOperation(),-1));
                buffl->getOperation()->cluster = cur_cluster;
              }
            }
          }  // End of needs-procssing if statement
        } // End of flow loop

      }     // End while stack not empty

    } // End of Operation loop

    // The above loop will visit ALL operations and recurse through the
    // buffers and resources connected to them.
    // Missing from the loop are buffers and resources that have no flows or
    // loads at all. We catch those poor lonely fellows now...
    for (Buffer::iterator gbuf2 = Buffer::begin();
        gbuf2 != Buffer::end(); ++gbuf2)
      if (gbuf2->getFlows().empty())
      {
        gbuf2->cluster = ++numberOfClusters;
        if (numberOfClusters >= USHRT_MAX)
          throw LogicException("Too many clusters");
        ++numberOfHangingClusters;
      }
    for (Resource::iterator gres2 = Resource::begin();
        gres2 != Resource::end(); ++gres2)
      if (gres2->getLoads().empty())
      {
        gres2->cluster = ++numberOfClusters;
        if (numberOfClusters >= USHRT_MAX)
          throw LogicException("Too many clusters");
        ++numberOfHangingClusters;
      }

  } // End of while recomputeLevels. The loop will be repeated as long as model
  // changes are done during the recomputation.

  // Unlock the exclusive access to this function
  computationBusy = false;
}

} // End Namespace
