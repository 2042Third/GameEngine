#include "stpch.h"
#include "Strata/Animation/Skeleton.h"

#include "Strata/Core/BinaryStream.h"

namespace Strata
{

	namespace
	{

		constexpr uint32_t c_SkeletonMagic = 0x4B535453; // "STSK"

		struct CookedSkeletonHeader
		{
			uint32_t Magic;
			uint32_t Version;
			uint32_t JointCount;
			uint32_t RootMotionJoint;
		};

		struct CookedJoint
		{
			uint32_t Parent; // c_NoJoint for root joints
			float Translation[3];
			float Rotation[4]; // x, y, z, w
			float Scale[3];
		};
		static_assert(sizeof(CookedJoint) == 44);

		bool IsFinite(const glm::vec3& value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
		}

		bool IsFinite(const glm::quat& value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) && std::isfinite(value.w);
		}

	}

	Ref<Skeleton> Skeleton::Create(std::vector<std::string> names, std::vector<JointIndex> parents, std::vector<JointPose> restPose, JointIndex rootMotionJoint,
		std::string* outError)
	{
		auto fail = [outError](const std::string& message) -> Ref<Skeleton>
		{
			if (outError)
				*outError = message;
			return nullptr;
		};

		const size_t count = names.size();
		if (count == 0)
			return fail("A skeleton needs at least one joint");
		if (count > c_MaxJoints)
			return fail(fmt::format("The skeleton has {} joints; at most {} are supported", count, c_MaxJoints));
		if (parents.size() != count || restPose.size() != count)
			return fail("The skeleton's joint names, parents and rest pose differ in length");

		for (size_t joint = 0; joint < count; joint++)
		{
			if (names[joint].empty() || names[joint].size() > c_MaxNameLength)
				return fail(fmt::format("Joint {} has an empty or too long name", joint));
			// Parents come first: a pose is computed in one pass over the joints.
			if (parents[joint] != c_NoJoint && parents[joint] >= joint)
				return fail(fmt::format("Joint '{}' comes before its parent", names[joint]));
			JointPose& pose = restPose[joint];
			if (!IsFinite(pose.Translation) || !IsFinite(pose.Scale) || !IsFinite(pose.Rotation))
				return fail(fmt::format("Joint '{}' has a non-finite rest pose", names[joint]));
			const float lengthSquared = glm::dot(pose.Rotation, pose.Rotation);
			if (!(lengthSquared > 1.0e-12f))
				return fail(fmt::format("Joint '{}' has a zero rotation", names[joint]));
			pose.Rotation = AnimationMath::Normalize(pose.Rotation);
		}

		if (rootMotionJoint == c_NoJoint)
			rootMotionJoint = 0; // Joint 0 is a root: its parent would have to come before it
		if (rootMotionJoint >= count)
			return fail(fmt::format("The root motion joint {} does not exist", rootMotionJoint));

		Ref<Skeleton> skeleton(new Skeleton());
		skeleton->m_Names = std::move(names);
		skeleton->m_Parents = std::move(parents);
		skeleton->m_RestPose = std::move(restPose);
		skeleton->m_RootMotionJoint = rootMotionJoint;
		skeleton->m_JointsByName.reserve(count);
		for (size_t joint = 0; joint < count; joint++)
		{
			if (!skeleton->m_JointsByName.emplace(skeleton->m_Names[joint], static_cast<JointIndex>(joint)).second)
				return fail(fmt::format("The joint name '{}' is used twice", skeleton->m_Names[joint]));
		}
		return skeleton;
	}

	std::vector<uint8_t> Skeleton::Serialize() const
	{
		BinaryWriter writer;
		writer.Write(CookedSkeletonHeader { c_SkeletonMagic, c_CookedVersion, GetJointCount(), m_RootMotionJoint });
		for (size_t joint = 0; joint < m_Names.size(); joint++)
		{
			const JointPose& pose = m_RestPose[joint];
			CookedJoint cooked = {};
			cooked.Parent = m_Parents[joint];
			for (int axis = 0; axis < 3; axis++)
			{
				cooked.Translation[axis] = pose.Translation[axis];
				cooked.Scale[axis] = pose.Scale[axis];
			}
			cooked.Rotation[0] = pose.Rotation.x;
			cooked.Rotation[1] = pose.Rotation.y;
			cooked.Rotation[2] = pose.Rotation.z;
			cooked.Rotation[3] = pose.Rotation.w;
			writer.Write(cooked);
		}
		for (const std::string& name : m_Names)
			writer.WriteString(name);
		return writer.TakeData();
	}

	Ref<Skeleton> Skeleton::Deserialize(std::span<const uint8_t> data, std::string* outError)
	{
		auto fail = [outError](const std::string& message) -> Ref<Skeleton>
		{
			if (outError)
				*outError = message;
			return nullptr;
		};

		BinaryReader reader(data);
		const CookedSkeletonHeader header = reader.Read<CookedSkeletonHeader>();
		if (!reader.IsValid() || header.Magic != c_SkeletonMagic)
			return fail("Not a cooked Strata skeleton");
		if (header.Version != c_CookedVersion)
			return fail(fmt::format("Unsupported cooked skeleton version {}", header.Version));
		if (header.JointCount == 0 || header.JointCount > c_MaxJoints || header.JointCount > reader.GetRemaining() / sizeof(CookedJoint)
			|| header.RootMotionJoint >= header.JointCount)
			return fail("Cooked skeleton header is corrupt");

		std::vector<JointIndex> parents(header.JointCount);
		std::vector<JointPose> restPose(header.JointCount);
		for (uint32_t joint = 0; joint < header.JointCount; joint++)
		{
			const CookedJoint cooked = reader.Read<CookedJoint>();
			if (cooked.Parent != c_NoJoint && cooked.Parent >= joint)
				return fail("Cooked skeleton joints are out of order");
			parents[joint] = static_cast<JointIndex>(cooked.Parent);
			JointPose& pose = restPose[joint];
			pose.Translation = glm::vec3(cooked.Translation[0], cooked.Translation[1], cooked.Translation[2]);
			pose.Rotation = glm::quat(cooked.Rotation[3], cooked.Rotation[0], cooked.Rotation[1], cooked.Rotation[2]);
			pose.Scale = glm::vec3(cooked.Scale[0], cooked.Scale[1], cooked.Scale[2]);
		}
		std::vector<std::string> names(header.JointCount);
		for (std::string& name : names)
			name = reader.ReadString(c_MaxNameLength);
		if (!reader.IsValid())
			return fail("Cooked skeleton is truncated");
		if (reader.GetRemaining() != 0)
			return fail("Cooked skeleton has trailing data");
		return Create(std::move(names), std::move(parents), std::move(restPose), static_cast<JointIndex>(header.RootMotionJoint), outError);
	}

	AssetMemoryUsage Skeleton::GetMemoryUsage() const
	{
		AssetMemoryUsage usage;
		usage.Cpu = sizeof(Skeleton) + m_Names.size() * (sizeof(std::string) + sizeof(JointIndex) + sizeof(JointPose) + 32);
		for (const std::string& name : m_Names)
			usage.Cpu += name.capacity();
		return usage;
	}

	JointIndex Skeleton::FindJoint(std::string_view name) const
	{
		auto it = m_JointsByName.find(name);
		return it != m_JointsByName.end() ? it->second : c_NoJoint;
	}

	void Skeleton::ComputeModelTransforms(std::span<const JointPose> local, std::span<glm::mat4> outModel) const
	{
		const size_t count = std::min({ local.size(), outModel.size(), m_Parents.size() });
		for (size_t joint = 0; joint < count; joint++)
		{
			const glm::mat4 matrix = AnimationMath::ToMatrix(local[joint]);
			const JointIndex parent = m_Parents[joint];
			outModel[joint] = parent == c_NoJoint ? matrix : AnimationMath::MultiplyAffine(outModel[parent], matrix);
		}
	}

}
