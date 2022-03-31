// SPDX-FileCopyrightText: 2021 Jorrit Rouwe
// SPDX-License-Identifier: MIT

#include <Jolt/Jolt.h>

#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/CollideShape.h>

JPH_NAMESPACE_BEGIN

CharacterVirtual::CharacterVirtual(CharacterVirtualSettings *inSettings, Vec3Arg inPosition, QuatArg inRotation, PhysicsSystem *inSystem) :
	mSystem(inSystem),
	mShape(inSettings->mShape)
{
	// Copy settings
	SetMaxSlopeAngle(inSettings->mMaxSlopeAngle);
	SetMaxStrength(inSettings->mMaxStrength);
	SetMass(inSettings->mMass);
	SetPenetrationRecoverySpeed(inSettings->mPenetrationRecoverySpeed);
}

template <class taCollector>
void CharacterVirtual::sFillContactProperties(Contact &outContact, const Body &inBody, const taCollector &inCollector, const CollideShapeResult &inResult)
{
	outContact.mPosition = inResult.mContactPointOn2;	
	outContact.mLinearVelocity = inBody.GetPointVelocity(inResult.mContactPointOn2);
	outContact.mNormal = -inResult.mPenetrationAxis.NormalizedOr(Vec3::sZero());
	outContact.mDistance = -inResult.mPenetrationDepth;
	outContact.mBodyB = inResult.mBodyID2;
	outContact.mSubShapeIDB = inResult.mSubShapeID2;
	outContact.mMotionTypeB = inBody.GetMotionType();
	outContact.mUserData = inBody.GetUserData();
	outContact.mMaterial = inCollector.GetContext()->GetMaterial(inResult.mSubShapeID2);
}

void CharacterVirtual::ContactCollector::AddHit(const CollideShapeResult &inResult)
{
	BodyLockRead lock(mSystem->GetBodyLockInterface(), inResult.mBodyID2);
	if (lock.SucceededAndIsInBroadPhase())
	{
		const Body &body = lock.GetBody();

		mContacts.push_back(Contact());
		Contact &contact = mContacts.back();
		sFillContactProperties(contact, body, *this, inResult);
		contact.mFraction = 0.0f;

		// Protection from excess of contact points
		if (mContacts.size() == cMaxNumHits)
			ForceEarlyOut();
	}
}

void CharacterVirtual::ContactCastCollector::AddHit(const ShapeCastResult &inResult)
{	
	if (inResult.mFraction > 0.0f // Ignore collisions at fraction = 0
		&& inResult.mPenetrationAxis.Dot(mDisplacement) > 0.0f) // Ignore penetrations that we're moving away from
	{
		// Test if this contact should be ignored
		for (const IgnoredContact &c : mIgnoredContacts)
			if (c.mBodyID == inResult.mBodyID2 && c.mSubShapeID == inResult.mSubShapeID2)
				return;

		BodyLockRead lock(mSystem->GetBodyLockInterface(), inResult.mBodyID2);
		if (lock.SucceededAndIsInBroadPhase())
		{
			const Body &body = lock.GetBody();

			mContacts.push_back(Contact());
			Contact &contact = mContacts.back();
			sFillContactProperties(contact, body, *this, inResult);
			contact.mFraction = inResult.mFraction;

			// Protection from excess of contact points
			if (mContacts.size() == cMaxNumHits)
				ForceEarlyOut();
		}
	}
}

void CharacterVirtual::GetContactsAtPosition(Vec3Arg inPosition, Vec3Arg inMovementDirection, const Shape *inShape, vector<Contact> &outContacts, const BroadPhaseLayerFilter &inBroadPhaseLayerFilter, const ObjectLayerFilter &inObjectLayerFilter, const BodyFilter &inBodyFilter) const
{
	// Remove previous results
	outContacts.clear();

	// Query shape transform
	Mat44 transform = Mat44::sRotation(mRotation);
	transform.SetTranslation(inPosition + transform.Multiply3x3(inShape->GetCenterOfMass()));

	// Settings for collide shape
	CollideShapeSettings settings;
	settings.mActiveEdgeMode = EActiveEdgeMode::CollideOnlyWithActive;
	settings.mBackFaceMode = EBackFaceMode::CollideWithBackFaces;
	settings.mActiveEdgeMovementDirection = inMovementDirection;
	settings.mMaxSeparationDistance = cPredictiveContactDistance;

	// Collide shape
	ContactCollector collector(mSystem, outContacts);
	mSystem->GetNarrowPhaseQuery().CollideShape(inShape, Vec3::sReplicate(1.0f), transform, settings, collector, inBroadPhaseLayerFilter, inObjectLayerFilter, inBodyFilter);

	// Reduce distance to contact by padding to ensure we stay away from the object by a little margin
	// (this will make collision detection cheaper - especially for sweep tests as they won't hit the surface if we're properly sliding)
	for (Contact &c : outContacts)
		c.mDistance -= cCharacterPadding;
}

void CharacterVirtual::RemoveConflictingContacts(vector<Contact> &ioContacts, vector<IgnoredContact> &outIgnoredContacts) const
{
	// Only use this algorithm if we're penetrating further than this (due to numerical precision issues we can always penetrate a little bit and we don't want to discard contacts if they just have a tiny penetration)
	// We do need to account for padding (see GetContactsAtPosition) that is removed from the contact distances, to compensate we add it to the cMinRequiredPenetration
	static constexpr float cMinRequiredPenetration = 0.005f + cCharacterPadding;

	// Discard conflicting penetrating contacts
	for (size_t c1 = 0; c1 < ioContacts.size(); c1++)
	{
		Contact &contact1 = ioContacts[c1];
		if (contact1.mDistance <= -cMinRequiredPenetration) // Only for penetrations
			for (size_t c2 = c1 + 1; c2 < ioContacts.size(); c2++)
			{
				Contact &contact2 = ioContacts[c2];
				if (contact1.mBodyB == contact2.mBodyB // Only same body
					&& contact2.mDistance <= -cMinRequiredPenetration // Only for penetrations
					&& contact1.mNormal.Dot(contact2.mNormal) < 0.0f) // Only opposing normals
				{
					// Discard contacts with the least amount of penetration
					if (contact1.mDistance < contact2.mDistance)
					{
						// Discard the 2nd contact
						outIgnoredContacts.emplace_back(contact2.mBodyB, contact2.mSubShapeIDB);
						ioContacts.erase(ioContacts.begin() + c2);
						c2--;
					}
					else
					{
						// Discard the first contact
						outIgnoredContacts.emplace_back(contact1.mBodyB, contact1.mSubShapeIDB);
						ioContacts.erase(ioContacts.begin() + c1);
						c1--;
						break;
					}
				}
			}
	}
}

bool CharacterVirtual::ValidateContact(const Contact &inContact) const
{
	if (mListener == nullptr)
		return true;

	return mListener->OnContactValidate(this, inContact.mBodyB, inContact.mSubShapeIDB);
}

bool CharacterVirtual::GetFirstContactForSweep(Vec3Arg inPosition, Vec3Arg inDisplacement, Contact &outContact, const vector<IgnoredContact> &inIgnoredContacts, const BroadPhaseLayerFilter &inBroadPhaseLayerFilter, const ObjectLayerFilter &inObjectLayerFilter, const BodyFilter &inBodyFilter) const
{
	// Too small distance -> skip checking
	if (inDisplacement.LengthSq() < 1.0e-8f)
		return false;

	// Calculate start transform
	Mat44 start = Mat44::sRotation(mRotation);
	start.SetTranslation(inPosition + start * mShape->GetCenterOfMass());

	// Settings for the cast
	ShapeCastSettings settings;
	settings.mBackFaceModeTriangles = EBackFaceMode::CollideWithBackFaces;
	settings.mBackFaceModeConvex = EBackFaceMode::IgnoreBackFaces;
	settings.mActiveEdgeMode = EActiveEdgeMode::CollideOnlyWithActive;
	settings.mUseShrunkenShapeAndConvexRadius = true;
	settings.mReturnDeepestPoint = false;

	// Cast shape
	vector<Contact> contacts;
	ContactCastCollector collector(mSystem, inDisplacement, inIgnoredContacts, contacts);
	ShapeCast shape_cast(mShape, Vec3::sReplicate(1.0f), start, inDisplacement);
	mSystem->GetNarrowPhaseQuery().CastShape(shape_cast, settings, collector, inBroadPhaseLayerFilter, inObjectLayerFilter, inBodyFilter);
	if (contacts.empty())
		return false;

	// Sort the contacts on fraction
	sort(contacts.begin(), contacts.end(), [](const Contact &inLHS, const Contact &inRHS) { return inLHS.mFraction < inRHS.mFraction; });

	// Check the first contact that will make us penetrate more than the allowed tolerance
	bool valid_contact = false;
	for (const Contact &c : contacts)
		if (c.mDistance + c.mNormal.Dot(inDisplacement) < -cCollisionTolerance
			&& ValidateContact(c))
		{
			outContact = c;
			valid_contact = true;
			break;
		}
	if (!valid_contact)
		return false;

	// Correct fraction for the padding that we want to keep from geometry
	// We want to maintain distance of cCharacterPadding (p) along plane normal outContact.mNormal (n) to the capsule by moving back along inDisplacement (d) by amount d'
	// cos(angle between d and -n) = -n dot d / |d| = p / d'
	// <=> d' = -p |d| / n dot d
	// The new fraction of collision is then:
	// f' = f - d' / |d| = f + p / n dot d
	outContact.mFraction = max(0.0f, outContact.mFraction + cCharacterPadding / outContact.mNormal.Dot(inDisplacement));
	return true;
}

void CharacterVirtual::DetermineConstraints(Vec3Arg inCharacterVelocity, vector<Contact> &inContacts, vector<Constraint> &outConstraints) const
{
	for (Contact &c : inContacts)
	{
		Vec3 contact_velocity = c.mLinearVelocity;

		// Penetrating contact: Add a contact velocity that pushes the character out at the desired speed
		if (c.mDistance < 0.0f)
			contact_velocity -= c.mNormal * c.mDistance * mPenetrationRecoverySpeed;

		// Determine relative velocity between character and contact
		Vec3 relative_velocity = inCharacterVelocity - contact_velocity;

		// Project the velocity on the normal
		float projected_velocity = c.mNormal.Dot(relative_velocity);
		if (projected_velocity >= 0.0f)
			continue; // Moving away from contact: Ignore

		// Convert to a constraint
		outConstraints.push_back(Constraint());
		Constraint &constraint = outConstraints.back();
		constraint.mContact = &c;
		constraint.mLinearVelocity = contact_velocity;
		constraint.mPlane = Plane(c.mNormal, c.mDistance);

		// Next check if the angle is too steep and if it is add an additional constraint that holds the character back
		if (mCosMaxSlopeAngle < 0.999f // If cos(slope angle) is close to 1 then there's no limit
			&& c.mNormal.GetY() >= 0.0f
			&& c.mNormal.GetY() < mCosMaxSlopeAngle)
		{
			// Make horizontal normal
			Vec3 normal = Vec3(c.mNormal.GetX(), 0.0f, c.mNormal.GetZ()).Normalized();

			// Create a secondary constraint that blocks horizontal movement
			outConstraints.push_back(Constraint());
			Constraint &vertical_constraint = outConstraints.back();
			vertical_constraint.mContact = &c;
			vertical_constraint.mLinearVelocity = contact_velocity.Dot(normal) * normal; // Project the contact velocity on the new normal so that both planes push at an equal rate
			vertical_constraint.mPlane = Plane(normal, c.mDistance / normal.Dot(c.mNormal)); // Calculate the distance we have to travel horizontally to hit the contact plane
		}
	}
}

bool CharacterVirtual::HandleContact(Vec3Arg inVelocity, Constraint &ioConstraint, Vec3Arg inGravity, float inDeltaTime) const
{
	Contact &contact = *ioConstraint.mContact;

	// Validate the contact point
	if (!ValidateContact(contact))
		return false;

	// Send contact added event
	CharacterContactSettings settings;
	if (mListener != nullptr)
		mListener->OnContactAdded(this, contact.mBodyB, contact.mSubShapeIDB, contact.mPosition, -contact.mNormal, settings);
	contact.mCanPushCharacter = settings.mCanPushCharacter;

	// If body B cannot receive an impulse, we're done
	if (!settings.mCanReceiveImpulses || contact.mMotionTypeB != EMotionType::Dynamic)
		return true;

	// Lock the body we're colliding with
	BodyLockWrite lock(mSystem->GetBodyLockInterface(), contact.mBodyB);
	if (!lock.SucceededAndIsInBroadPhase())
		return false; // Body has been removed, we should not collide with it anymore
	const Body &body = lock.GetBody();

	// Calculate the velocity that we want to apply at B so that it will start moving at the character's speed at the contact point
	constexpr float cDamping = 0.9f;
	constexpr float cPenetrationResolution = 0.4f;
	Vec3 relative_velocity = inVelocity - contact.mLinearVelocity;
	float projected_velocity = relative_velocity.Dot(contact.mNormal);
	float delta_velocity = -projected_velocity * cDamping - min(contact.mDistance, 0.0f) * cPenetrationResolution / inDeltaTime;

	// Don't apply impulses if we're separating
	if (delta_velocity < 0.0f)
		return true;

	// Determine mass properties of the body we're colliding with
	const MotionProperties *motion_properties = body.GetMotionProperties();
	Vec3 center_of_mass = body.GetCenterOfMassPosition();
	Mat44 inverse_inertia = body.GetInverseInertia();
	float inverse_mass = motion_properties->GetInverseMass();

	// Calculate the inverse of the mass of body B as seen at the contact point in the direction of the contact normal
	Vec3 jacobian = (contact.mPosition - center_of_mass).Cross(contact.mNormal);
	float inv_effective_mass = inverse_inertia.Multiply3x3(jacobian).Dot(jacobian) + inverse_mass;

	// Impulse P = M dv
	float impulse = delta_velocity / inv_effective_mass;

	// Clamp the impulse according to the character strength, character strength is a force in newtons, P = F dt
	float max_impulse = mMaxStrength * inDeltaTime;
	impulse = min(impulse, max_impulse);

	// Calculate the world space impulse to apply
	Vec3 world_impulse = -impulse * contact.mNormal;

	// Add the impulse due to gravity working on the player: P = F dt = M g dt
	float normal_dot_gravity = contact.mNormal.Dot(inGravity);
	if (normal_dot_gravity < 0.0f)
		world_impulse -= (mMass * normal_dot_gravity / inGravity.Length() * inDeltaTime) * inGravity;

	// Now apply the impulse (body is already locked so we use the no-lock interface)
	mSystem->GetBodyInterfaceNoLock().AddImpulse(contact.mBodyB, world_impulse, contact.mPosition);
	return true;
}

void CharacterVirtual::SolveConstraints(Vec3Arg inVelocity, Vec3Arg inGravity, float inDeltaTime, float inTimeRemaining, vector<Constraint> &ioConstraints, float &outTimeSimulated, Vec3 &outDisplacement) const
{
	// If there are no constraints we can immediately move to our target
	if (ioConstraints.empty())
	{
		outDisplacement = inVelocity * inTimeRemaining;
		outTimeSimulated = inTimeRemaining;
		return;
	}

	// Create array that holds the constraints in order of time of impact (sort will happen later)
	vector<Constraint *> sorted_constraints;
	sorted_constraints.resize(ioConstraints.size());
	for (size_t index = 0; index < sorted_constraints.size(); index++)
		sorted_constraints[index] = &ioConstraints[index];

	// This is the velocity we use for the displacement, if we hit something it will be shortened
	Vec3 velocity = inVelocity;

	// Start with no displacement
	outDisplacement = Vec3::sZero();
	outTimeSimulated = 0.0f;

	// These are the contacts that we hit previously without moving a significant distance
	Constraint *previous_contacts[cMaxConstraintIterations];
	int num_previous_contacts = 0;

	// Loop for a max amount of iterations
	for (int iteration = 0; iteration < cMaxConstraintIterations; iteration++)
	{
		// Calculate time of impact for all constraints
		for (Constraint &c : ioConstraints)
		{
			// Project velocity on plane direction
			c.mProjectedVelocity = c.mPlane.GetNormal().Dot(c.mLinearVelocity - velocity);
			if (c.mProjectedVelocity < 1.0e-6f)
			{
				c.mTOI = FLT_MAX;
			}
			else
			{
				// Distance to plane
				float dist = c.mPlane.SignedDistance(outDisplacement);

				if (dist - c.mProjectedVelocity * inTimeRemaining > -1.0e-4f)
				{
					// Too little penetration, accept the movement
					c.mTOI = FLT_MAX;
				}
				else
				{
					// Calculate time of impact
					c.mTOI = max(0.0f, dist / c.mProjectedVelocity);
				}
			}
		}
				
		// Sort constraints on proximity
		sort(sorted_constraints.begin(), sorted_constraints.end(), [](Constraint *inLHS, Constraint *inRHS) {
				// If both constraints hit at t = 0 then order the one that will push the character furthest first
				// Note that because we add velocity to penetrating contacts, this will also resolve contacts that penetrate the most
				if (inLHS->mTOI <= 0.0f && inRHS->mTOI <= 0.0f)
					return inLHS->mProjectedVelocity > inRHS->mProjectedVelocity;

				// Then sort on time of impact
				if (inLHS->mTOI != inRHS->mTOI)
					return inLHS->mTOI < inRHS->mTOI;

				// As a tie breaker sort static first so it has the most influence
				return inLHS->mContact->mMotionTypeB > inRHS->mContact->mMotionTypeB;
			});

		// Find the first valid constraint
		Constraint *constraint = nullptr;
		for (Constraint *c : sorted_constraints)
		{
			// Take the first contact and see if we can reach it
			if (c->mTOI >= inTimeRemaining)
			{
				// We can reach our goal!
				outDisplacement += velocity * inTimeRemaining;
				outTimeSimulated += inTimeRemaining;
				return;
			}

			// Test if this contact was discarded by the contact callback before
			if (c->mContact->mWasDiscarded)
				continue;

			// Check if we made contact with this before
			if (!c->mContact->mHadCollision)
			{
				// Handle the contact
				if (!HandleContact(velocity, *c, inGravity, inDeltaTime))
				{
					// Constraint should be ignored, remove it from the list
					c->mContact->mWasDiscarded = true;
					continue;
				}

				c->mContact->mHadCollision = true;
			}

			// Cancel velocity of constraint if it cannot push the character
			if (!c->mContact->mCanPushCharacter)
				c->mLinearVelocity = Vec3::sZero();

			// We found the first constraint that we want to collide with
			constraint = c;
			break;
		}

		if (constraint == nullptr)
		{
			// All constraints were discarded, we can reach our goal!
			outDisplacement += velocity * inTimeRemaining;
			outTimeSimulated += inTimeRemaining;
			return;
		}

		// Move to the contact
		outDisplacement += velocity * constraint->mTOI;
		inTimeRemaining -= constraint->mTOI;
		outTimeSimulated += constraint->mTOI;

		// If there's not enough time left to be simulated, bail
		if (inTimeRemaining < cMinTimeRemaining)
			return;

		// If we've moved significantly, clear all previous contacts
		if (constraint->mTOI > 1.0e-4f)
			num_previous_contacts = 0;

		// Get the normal of the plane we're hitting
		Vec3 plane_normal = constraint->mPlane.GetNormal();

		// Get the relative velocity between the character and the constraint
		Vec3 relative_velocity = velocity - constraint->mLinearVelocity;

		// Calculate new velocity if we cancel the relative velocity in the normal direction
		Vec3 new_velocity = velocity - relative_velocity.Dot(plane_normal) * plane_normal;

		// Find the normal of the previous contact that we will violate the most if we move in this new direction
		float highest_penetration = 0.0f;
		Constraint *other_constraint = nullptr;
		for (Constraint **c = previous_contacts; c < previous_contacts + num_previous_contacts; ++c)
			if (*c != constraint)
			{
				// Calculate how much we will penetrate if we move in this direction
				Vec3 other_normal = (*c)->mPlane.GetNormal();
				float penetration = ((*c)->mLinearVelocity - new_velocity).Dot(other_normal);
				if (penetration > highest_penetration)
				{
					// We don't want parallel or anti-parallel normals as that will cause our cross product below to become zero. Slack is approx 10 degrees.
					float dot = other_normal.Dot(plane_normal);
					if (dot < 0.984f && dot > -0.984f) 
					{
						highest_penetration = penetration;
						other_constraint = *c;
					}
				}
			}

		// Check if we found a 2nd constraint
		if (other_constraint != nullptr)
		{
			// Calculate the sliding direction and project the new velocity onto that sliding direction
			Vec3 other_normal = other_constraint->mPlane.GetNormal();
			Vec3 slide_dir = plane_normal.Cross(other_normal).Normalized();
			Vec3 velocity_in_slide_dir = new_velocity.Dot(slide_dir) * slide_dir;

			// Cancel the constraint velocity in the other constraint plane's direction so that we won't try to apply it again and keep ping ponging between planes
			constraint->mLinearVelocity -= min(0.0f, constraint->mLinearVelocity.Dot(other_normal)) * other_normal;

			// Cancel the other constraints velocity in this constraint plane's direction so that we won't try to apply it again and keep ping ponging between planes
			other_constraint->mLinearVelocity -= min(0.0f, other_constraint->mLinearVelocity.Dot(plane_normal)) * plane_normal;

			// Calculate the velocity of this constraint perpendicular to the slide direction
			Vec3 perpendicular_velocity = constraint->mLinearVelocity - constraint->mLinearVelocity.Dot(slide_dir) * slide_dir;

			// Calculate the velocity of the other constraint perpendicular to the slide direction
			Vec3 other_perpendicular_velocity = other_constraint->mLinearVelocity - other_constraint->mLinearVelocity.Dot(slide_dir) * slide_dir;

			// Add all components together
			velocity = velocity_in_slide_dir + perpendicular_velocity + other_perpendicular_velocity;
		}			
		else
		{
			// Update the velocity
			velocity = new_velocity;
		}

		// Add the contact to the list so that next iteration we can avoid violating it again
		previous_contacts[num_previous_contacts] = constraint;
		num_previous_contacts++;

		// If there's not enough velocity left, bail
		if (velocity.LengthSq() < 1.0e-8f)
			return;
	}
}

void CharacterVirtual::UpdateSupportingContact()
{
	// Flag contacts as having a collision if they're close enough.
	// Note that if we did MoveShape before we want to preserve any contacts that it marked as colliding
	for (Contact &c : mActiveContacts)
		if (!c.mWasDiscarded)
			c.mHadCollision |= c.mDistance < cCollisionTolerance;

	// Find the contact with the normal that is pointing most upwards and store it in mSupportingContact
	mSupportingContact = nullptr;
	float max_y = -FLT_MAX;
	for (const Contact &c : mActiveContacts)
		if (c.mHadCollision && max_y < c.mNormal.GetY())
		{
			mSupportingContact = &c;
			max_y = c.mNormal.GetY();
		}
}

void CharacterVirtual::StoreActiveContacts(const vector<Contact> &inContacts)
{
	mActiveContacts = inContacts;

	UpdateSupportingContact();
}

void CharacterVirtual::MoveShape(Vec3 &ioPosition, Vec3Arg inVelocity, Vec3Arg inGravity, float inDeltaTime, vector<Contact> *outActiveContacts, const BroadPhaseLayerFilter &inBroadPhaseLayerFilter, const ObjectLayerFilter &inObjectLayerFilter, const BodyFilter &inBodyFilter)
{
	// Calculate starting position for the shape
	Vec3 position = GetShapePosition(ioPosition);

	Vec3 movement_direction = inVelocity.NormalizedOr(Vec3::sZero());

	float time_remaining = inDeltaTime;
	for (int iteration = 0; iteration < cMaxCollisionIterations && time_remaining >= cMinTimeRemaining; iteration++)
	{
		// Determine contacts in the neighborhood
		// TODO: Query the broadphase only once instead of for every iteration
		vector<Contact> contacts;
		GetContactsAtPosition(position, movement_direction, mShape, contacts, inBroadPhaseLayerFilter, inObjectLayerFilter, inBodyFilter);

		// Remove contacts with the same body that have conflicting normals
		vector<IgnoredContact> ignored_contacts;
		ignored_contacts.reserve(contacts.size());
		RemoveConflictingContacts(contacts, ignored_contacts);

		// Convert contacts into constraints
		vector<Constraint> constraints;
		constraints.reserve(contacts.size() * 2);
		DetermineConstraints(inVelocity, contacts, constraints);

		// Solve the displacement using these constraints
		Vec3 displacement;
		float time_simulated;
		SolveConstraints(inVelocity, inGravity, inDeltaTime, time_remaining, constraints, time_simulated, displacement);

		// Store the contacts now that the colliding ones have been marked
		if (outActiveContacts != nullptr)
			*outActiveContacts = contacts;

		// Do a sweep to test if the path is really unobstructed
		Contact cast_contact;
		if (GetFirstContactForSweep(position, displacement, cast_contact, ignored_contacts, inBroadPhaseLayerFilter, inObjectLayerFilter, inBodyFilter))
		{
			displacement *= cast_contact.mFraction;
			time_simulated *= cast_contact.mFraction;
		}

		// Update the position
		ioPosition += displacement;
		position += displacement;
		time_remaining -= time_simulated;

		// If the displacement during this iteration was too small we assume we cannot further progress this update
		if (displacement.LengthSq() < 1.0e-8f)
			break;
	}
}

void CharacterVirtual::Update(float inDeltaTime, Vec3Arg inGravity, const BroadPhaseLayerFilter &inBroadPhaseLayerFilter, const ObjectLayerFilter &inObjectLayerFilter, const BodyFilter &inBodyFilter)
{
	// Slide the shape through the world
	Vec3 old_position = mPosition;
	MoveShape(mPosition, mLinearVelocity, inGravity, inDeltaTime, &mActiveContacts, inBroadPhaseLayerFilter, inObjectLayerFilter, inBodyFilter);

	// Update velocity based on motion
	mLinearVelocity = (mPosition - old_position) / inDeltaTime;

	// Determine the object that we're standing on
	UpdateSupportingContact();
}

void CharacterVirtual::RefreshContacts(const BroadPhaseLayerFilter &inBroadPhaseLayerFilter, const ObjectLayerFilter &inObjectLayerFilter, const BodyFilter &inBodyFilter)
{
	// Calculate position for the shape
	Vec3 position = GetShapePosition(mPosition);

	// Determine the contacts
	vector<Contact> contacts;
	GetContactsAtPosition(position, mLinearVelocity.NormalizedOr(Vec3::sZero()), mShape, contacts, inBroadPhaseLayerFilter, inObjectLayerFilter, inBodyFilter);

	StoreActiveContacts(contacts);
}

bool CharacterVirtual::SetShape(const Shape *inShape, float inMaxPenetrationDepth, const BroadPhaseLayerFilter &inBroadPhaseLayerFilter, const ObjectLayerFilter &inObjectLayerFilter, const BodyFilter &inBodyFilter)
{
	if (mShape == nullptr || mSystem == nullptr)
	{
		// It hasn't been initialized yet
		mShape = inShape;
		return true;
	}

	if (inShape != mShape && inShape != nullptr)
	{
		// Check collision around the new shape
		vector<Contact> contacts;
		GetContactsAtPosition(GetShapePosition(mPosition), mLinearVelocity.NormalizedOr(Vec3::sZero()), inShape, contacts, inBroadPhaseLayerFilter, inObjectLayerFilter, inBodyFilter);

		if (inMaxPenetrationDepth < FLT_MAX)
		{
			// Test if this results in penetration of the unpadded shape, if so cancel the transition
			for (const Contact &c : contacts)
				if (c.mDistance < -inMaxPenetrationDepth)
					return false;
		}

		// Store the new shape
		mShape = inShape;

		StoreActiveContacts(contacts);
	}

	return mShape == inShape;
}

CharacterVirtual::EGroundState CharacterVirtual::GetGroundState() const
{
	if (mSupportingContact == nullptr)
		return EGroundState::InAir;

	if (mCosMaxSlopeAngle < 0.999f // If cos(slope angle) is close to 1 then there's no limit
		&& mSupportingContact->mNormal.GetY() >= 0.0f
		&& mSupportingContact->mNormal.GetY() < mCosMaxSlopeAngle)
		return EGroundState::Sliding;

	return EGroundState::OnGround;
}

JPH_NAMESPACE_END
