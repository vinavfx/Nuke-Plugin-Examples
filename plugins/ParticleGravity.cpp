// Copyright (c) 2011 The Foundry Visionmongers Ltd.  All Rights Reserved.

#include "DDImage/Knob.h"
#include "DDImage/Knobs.h"
#include "DDImage/ParticleOp.h"
#include "DDImage/ParallelFor.h"
#include <array>

using namespace DD::Image;

static const char* const CLASS = "ParticleGravity";
static const char* const HELP =
    "Apply gravity to the particles. \n"
    "- can be applied on each or all of the x,y,z axis in both directions.\n"
    "- is actually more of a directional acceleration node than gravity (it "
    "can go in directions other than \"down\").";

class ParticleGravity : public ParticleBehaviour
{
public:
  ParticleGravity(Node* node) : ParticleBehaviour(node)
  {
    _forceAxis = {{ Vector3(0, 0, 0), Vector3(0, -0.1, 0) }};
  }

  void knobs( Knob_Callback f ) override
  {
    ParticleBehaviour::knobs(f);
    Divider(f);
    PositionVector_knob(f, reinterpret_cast<float*>(_forceAxis.data()), "strength");
    Tooltip(f, "The acceleration to be applied to all particles, expressed in units/frame/frame. "
            "The direction and magnitude of the vector are applied. The position of the vector "
            "has no effect.\n"
            "Gravity affects all particles equally irrespective of mass.  For more details "
            "see Galileo. ");
    addConditionsKnobs( f );
    addDomainKnobs( f );

    if (!isLicensed())
      set_unlicensed();
  }

  bool applyBehaviour( const ParticleContext& context, ParticleSystem* ps ) override
  {
    Vector3 strength = _forceAxis[1] - _forceAxis[0];

    const auto particleStartTime = ps->particleStartTime();
    auto particleVelocity = ps->particleVelocity();

    ParallelFor(ps->numParticles(),
      [&](int i) {
        if ( conditionsApply( ps, i ) )
          applyAcceleration(context, strength, particleVelocity[i], particleStartTime[i]);
      }
    );

    return true;
  }

  static const Description description;
  const char* Class() const override { return CLASS; }
  const char* node_help() const override { return HELP; }

private:
  std::array<Vector3, 2> _forceAxis;
};

static Op* build(Node* node) { return new ParticleGravity(node); }
const Op::Description ParticleGravity::description(CLASS, build);
