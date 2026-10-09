// Scripts of the Scripting.Collisions tests (StrataTests/src/Scripting/ScriptCollisionTests.cpp): contact callbacks, and
// scripts that destroy entities, remove scripts or throw while they handle one.

#include "TestScripts.h"

#include <cstdint>
#include <stdexcept>

using namespace Strata;
using namespace ScriptTests;

// Counts the contact callbacks of its entity, keeps the last contact and records every callback in the "Log" entity.
class ContactRecorder : public Script
{
public:
	int32_t CollisionEnters = 0;
	int32_t CollisionExits = 0;
	int32_t TriggerEnters = 0;
	int32_t TriggerExits = 0;
	Entity LastOther;
	glm::vec3 LastPoint = glm::vec3(0.0f);
	glm::vec3 LastNormal = glm::vec3(0.0f);
	bool LastOtherValid = false; // Whether the other entity existed when the last contact was reported

	void OnCollisionEnter(const Collision& collision) override
	{
		CollisionEnters++;
		Keep(collision, "CollisionEnter");
	}

	void OnCollisionExit(const Collision& collision) override
	{
		CollisionExits++;
		Keep(collision, "CollisionExit");
	}

	void OnTriggerEnter(const Collision& collision) override
	{
		TriggerEnters++;
		Keep(collision, "TriggerEnter");
	}

	void OnTriggerExit(const Collision& collision) override
	{
		TriggerExits++;
		Keep(collision, "TriggerExit");
	}
private:
	void Keep(const Collision& collision, const char* event)
	{
		LastOther = collision.Other;
		LastPoint = collision.Point;
		LastNormal = collision.Normal;
		LastOtherValid = collision.Other.IsValid();
		Record(*this, "ContactRecorder", event);
	}
};

ST_SCRIPT_CLASS(ContactRecorder)
{
	ST_SCRIPT_FIELD(CollisionEnters);
	ST_SCRIPT_FIELD(CollisionExits);
	ST_SCRIPT_FIELD(TriggerEnters);
	ST_SCRIPT_FIELD(TriggerExits);
	ST_SCRIPT_FIELD(LastOther);
	ST_SCRIPT_FIELD(LastPoint);
	ST_SCRIPT_FIELD(LastNormal);
	ST_SCRIPT_FIELD(LastOtherValid);
}

// Destroys its own entity when it first touches something.
class DestroySelfOnContact : public Script
{
public:
	int32_t Contacts = 0;

	void OnCollisionEnter(const Collision&) override
	{
		Contacts++;
		Record(*this, "DestroySelfOnContact", "CollisionEnter");
		GetEntity().Destroy();
	}

	void OnDestroy() override
	{
		Record(*this, "DestroySelfOnContact", "Destroy");
	}
};

ST_SCRIPT_CLASS(DestroySelfOnContact)
{
	ST_SCRIPT_FIELD(Contacts);
}

// Destroys the first entity it touches, then reports the end of that contact (the entity is gone by then).
class DestroyOtherOnContact : public Script
{
public:
	int32_t Contacts = 0;
	int32_t Exits = 0;
	Entity Destroyed;
	bool ExitOtherValid = true;

	void OnCollisionEnter(const Collision& collision) override
	{
		Record(*this, "DestroyOtherOnContact", "CollisionEnter");
		if (Contacts++ > 0)
			return;
		Destroyed = collision.Other;
		Destroyed.Destroy();
	}

	void OnCollisionExit(const Collision& collision) override
	{
		if (collision.Other != Destroyed)
			return;
		Exits++;
		ExitOtherValid = collision.Other.IsValid();
		Record(*this, "DestroyOtherOnContact", "CollisionExit");
	}
};

ST_SCRIPT_CLASS(DestroyOtherOnContact)
{
	ST_SCRIPT_FIELD(Contacts);
	ST_SCRIPT_FIELD(Exits);
	ST_SCRIPT_FIELD(Destroyed);
	ST_SCRIPT_FIELD(ExitOtherValid);
}

// Removes itself from its entity when it first touches something: it receives no further callbacks.
class RemoveSelfOnContact : public Script
{
public:
	int32_t Calls = 0;

	void OnCollisionEnter(const Collision&) override
	{
		Calls++;
		GetEntity().RemoveScript("RemoveSelfOnContact");
	}

	void OnCollisionExit(const Collision&) override
	{
		Calls++;
		Record(*this, "RemoveSelfOnContact", "CollisionExit");
	}

	void OnDestroy() override
	{
		Record(*this, "RemoveSelfOnContact", "Destroy");
	}
};

ST_SCRIPT_CLASS(RemoveSelfOnContact)
{
	ST_SCRIPT_FIELD(Calls);
}

// Throws from its first contact callback: the engine disables the script.
class ContactThrower : public Script
{
public:
	int32_t Calls = 0;

	void OnCollisionEnter(const Collision&) override
	{
		Calls++;
		throw std::runtime_error("Contact handling failed on purpose");
	}

	void OnCollisionExit(const Collision&) override
	{
		Calls++;
	}
};

ST_SCRIPT_CLASS(ContactThrower)
{
	ST_SCRIPT_FIELD(Calls);
}

// Counts contacts without calling the engine (the tests call it directly, outside any scene).
class ContactCounter : public Script
{
public:
	int32_t Contacts = 0;
	glm::vec3 LastNormal = glm::vec3(0.0f);

	void OnCollisionEnter(const Collision& collision) override
	{
		Contacts++;
		LastNormal = collision.Normal;
	}
};

ST_SCRIPT_CLASS(ContactCounter)
{
	ST_SCRIPT_FIELD(Contacts);
	ST_SCRIPT_FIELD(LastNormal);
}
