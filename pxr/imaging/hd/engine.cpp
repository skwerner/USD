//
// Copyright 2016 Pixar
//
// Licensed under the Apache License, Version 2.0 (the "Apache License")
// with the following modification; you may not use this file except in
// compliance with the Apache License and the following modification to it:
// Section 6. Trademarks. is deleted and replaced with:
//
// 6. Trademarks. This License does not grant permission to use the trade
//    names, trademarks, service marks, or product names of the Licensor
//    and its affiliates, except as required to comply with Section 4(c) of
//    the License and to reproduce the content of the NOTICE file.
//
// You may obtain a copy of the Apache License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the Apache License with the above modification is
// distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied. See the Apache License for the specific
// language governing permissions and limitations under the Apache License.
//
#include "pxr/imaging/hd/engine.h"

#include "pxr/imaging/hd/debugCodes.h"
#include "pxr/imaging/hd/driver.h"
#include "pxr/imaging/hd/material.h"
#include "pxr/imaging/hd/perfLog.h"
#include "pxr/imaging/hd/renderDelegate.h"
#include "pxr/imaging/hd/renderIndex.h"
#include "pxr/imaging/hd/renderPass.h"
#include "pxr/imaging/hd/renderPassState.h"
#include "pxr/imaging/hd/resourceRegistry.h"
#include "pxr/imaging/hd/rprim.h"
#include "pxr/imaging/hd/task.h"
#include "pxr/imaging/hd/tokens.h"

#include <sstream>

PXR_NAMESPACE_OPEN_SCOPE


HdEngine::HdEngine() 
 : _taskContext()
{
}

HdEngine::~HdEngine()
{
}

void 
HdEngine::SetTaskContextData(const TfToken &id, const VtValue &data)
{
    // See if the token exists in the context and if not add it.
    std::pair<HdTaskContext::iterator, bool> result =
                                                 _taskContext.emplace(id, data);
    if (!result.second) {
        // Item wasn't new, so need to update it
        result.first->second = data;
    }
}

bool
HdEngine::GetTaskContextData(const TfToken &id, VtValue *data) const
{
    if (!TF_VERIFY(data)) {
        return false;
    }

    auto const& it = _taskContext.find(id);
    if (it != _taskContext.end()) {
        *data = it->second;
        return true;
    }

    return false;
}

void
HdEngine::RemoveTaskContextData(const TfToken &id)
{
    _taskContext.erase(id);
}

void
HdEngine::ClearTaskContextData()
{
    _taskContext.clear();
}

void
HdEngine::Execute(HdRenderIndex *index, HdTaskSharedPtrVector *tasks)
{
    TRACE_FUNCTION();

    if ((index == nullptr) || (tasks == nullptr)) {
        TF_CODING_ERROR("Passed nullptr to HdEngine::Execute()");
        return;
    }

    // Some render tasks may need access to the same rendering context / driver
    // as the render delegate. For example some tasks use Hgi.
    _taskContext[HdTokens->drivers] = VtValue(index->GetDrivers());

    // --------------------------------------------------------------------- //
    // DATA DISCOVERY PHASE
    // --------------------------------------------------------------------- //
    // Discover all required input data needed to render the required render
    // prim representations. At this point, we must read enough data to
    // establish the resource dependency graph, but we do not yet populate CPU-
    // or GPU-memory with data.

    // As a result of the next call, the resource registry will be populated
    // with both BufferSources that need to be resolved (possibly generating
    // data on the CPU) and computations to run on the CPU/GPU.

    TF_DEBUG(HD_ENGINE_PHASE_INFO).Msg(
            "\n"
            "==============================================================\n"
            "      HdEngine [Data Discovery Phase](RenderIndex::SyncAll)   \n"
            "--------------------------------------------------------------\n");
    index->SyncAll(tasks, &_taskContext);

    const size_t numTasks = tasks->size();

    // --------------------------------------------------------------------- //
    // PREPARE PHASE
    // --------------------------------------------------------------------- //
    // Now that all Prims have obtained obtained their current states
    // we can now prepare the task system for rendering.
    //
    // While sync operations are change-tracked, so are only performed if
    // something is dirty, prepare operations are done for every execution.
    //
    // As tasks are synced first, they cannot resolve their bindings at sync
    // time, so this is where tasks perform their inter-prim communication.
    //
    // The prepare phase is also where a task manages the resources it needs
    // for the render phase.
    TF_DEBUG(HD_ENGINE_PHASE_INFO).Msg(
            "\n"
            "==============================================================\n"
            "             HdEngine [Prepare Phase](Task::Prepare)          \n"
            "--------------------------------------------------------------\n");
    {
        TRACE_FUNCTION_SCOPE("Task Prepare");
        for (size_t taskNum = 0; taskNum < numTasks; ++taskNum) {
            const HdTaskSharedPtr &task = (*tasks)[taskNum];

            task->Prepare(&_taskContext, index);
        }
    }

    // --------------------------------------------------------------------- //
    // DATA COMMIT PHASE
    // --------------------------------------------------------------------- //
    // Having acquired handles to the data needed to update various resources,
    // we let the render delegate 'commit' these resources. These resources may
    // reside either on the CPU/GPU/both; that depends on the render delegate
    // implementation.
    TF_DEBUG(HD_ENGINE_PHASE_INFO).Msg(
            "\n"
            "==============================================================\n"
            " HdEngine [Data Commit Phase](RenderDelegate::CommitResources)\n"
            "--------------------------------------------------------------\n");
    HdRenderDelegate *renderDelegate = index->GetRenderDelegate();
    renderDelegate->CommitResources(&index->GetChangeTracker());

    // --------------------------------------------------------------------- //
    // EXECUTE PHASE
    // --------------------------------------------------------------------- //
    // Having updated all the necessary data buffers, we can finally execute
    // the rendering tasks.
    {
        TRACE_FUNCTION_SCOPE("Task Execution");
        TF_DEBUG(HD_ENGINE_PHASE_INFO).Msg(
            "\n"
            "==============================================================\n"
            "             HdEngine [Execute Phase](Task::Execute)          \n"
            "--------------------------------------------------------------\n");

        for (size_t taskNum = 0; taskNum < numTasks; ++taskNum) {
            const HdTaskSharedPtr &task = (*tasks)[taskNum];

            task->Execute(&_taskContext);
        }
    }
}

PXR_NAMESPACE_CLOSE_SCOPE
