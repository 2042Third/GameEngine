#include "Include/Common.slang"

#include "Include/Culling.slang"

// After Cull.comp's counting passes: packs the visibility feedback of the touched mesh and material slots into records
// (RenderData::FeedbackRecord) the CPU reads back (SceneRenderer::ApplyFeedback): the header record (the touched slots'
// counts), then MeshSlotCount mesh records and MaterialSlotCount material records, the touched ones first in each part.
// One thread per record place.

void st_Main()
{
	uint index = st_GlobalInvocationID.x;
	uint meshes = u_FeedbackLists[0];
	uint materials = u_FeedbackLists[1];
	if (index == 0u)
		u_FeedbackRecords[0] = uint4(meshes, materials, 0u, 0u);
	if (index < u_Cull.MeshSlotCount)
	{
		if (index >= meshes)
			return;
		uint slot = u_FeedbackLists[ST_FEEDBACK_LIST_HEADER + index];
		u_FeedbackRecords[1u + index] = uint4(slot, u_MeshFeedback[slot * 2u + 1u], u_MeshFeedback[slot * 2u], 0u);
		return;
	}
	uint place = index - u_Cull.MeshSlotCount;
	if (place >= u_Cull.MaterialSlotCount || place >= materials)
		return;
	uint slot = u_FeedbackLists[ST_FEEDBACK_LIST_HEADER + u_Cull.MeshSlotCount + place];
	uint base = slot * 4u;
	u_FeedbackRecords[1u + u_Cull.MeshSlotCount + place] = uint4(slot, u_MaterialFeedback[base + 2u], u_MaterialFeedback[base], u_MaterialFeedback[base + 1u]);
}

// --- Entry point ---
[numthreads(64, 1, 1)]
void main(uint3 globalID : SV_DispatchThreadID, uint3 localID : SV_GroupThreadID, uint3 groupID : SV_GroupID, uint localIndex : SV_GroupIndex)
{
	st_GlobalInvocationID = globalID;
	st_LocalInvocationID = localID;
	st_WorkGroupID = groupID;
	st_LocalInvocationIndex = localIndex;
	st_Main();
}
