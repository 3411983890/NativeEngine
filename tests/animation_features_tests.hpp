#pragma once
#include <cmath>
#include <stdexcept>
// Geometric and continuation tests exercise public runtime APIs, independent of
// the implementation's helper functions and selected fixture poses.
inline nlohmann::json
featureTests(const std::shared_ptr<const engine::animation::Asset> &asset) {
  using namespace engine::animation;
  unsigned checks = 0;
  auto check = [&](bool ok, const char *reason) {
    ++checks;
    if (!ok)
      throw std::runtime_error(reason);
  };
  auto distance = [](V3 a, V3 b) {
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) +
                     (a.z - b.z) * (a.z - b.z));
  };
  auto origin = [](const Matrix &m) { return V3{m[12], m[13], m[14]}; };
  auto hash = [](const Controller &c) {
    engine::determinism::Hasher h;
    c.digest(h);
    return h.value();
  };
  check(asset->rig.legs.size() == 2 && asset->morphs.size() == 2 &&
            asset->softRegions.size() == 2,
        "Feature asset was not imported");
  auto pose = asset->skeleton.restPose();
  const auto chain = asset->rig.legs[0];
  auto before = asset->skeleton.globals(pose);
  V3 target{-.28f, .28f, -.19f};
  auto ik = solveTwoBone(asset->skeleton, pose, chain, target, {-.5f, .5f, -1});
  auto after = asset->skeleton.globals(pose);
  check(ik.error < .0002f &&
            distance(origin(after[chain.tip]), target) < .0002f,
        "Two bone IK reachable target");
  check(std::abs(
            distance(origin(before[chain.root]), origin(before[chain.joint])) -
            distance(origin(after[chain.root]), origin(after[chain.joint]))) <
            1e-5f,
        "IK changed upper bone length");
  check(std::abs(
            distance(origin(before[chain.tip]), origin(before[chain.joint])) -
            distance(origin(after[chain.tip]), origin(after[chain.joint]))) <
            1e-5f,
        "IK changed lower bone length");
  check(pose.translation == asset->skeleton.restPose().translation,
        "IK must use rotations without bone stretching");
  auto far =
      solveTwoBone(asset->skeleton, pose, chain, {0, -10, 0}, {0, 0, -1});
  check(far.clamped && far.error > 5,
        "IK unreachable target must report residual");
  pose = asset->skeleton.restPose();
  solveLookAt(asset->skeleton, pose, unsigned(asset->rig.lookBone), {2, 2, -2},
              .4f);
  check(pose.rotation[unsigned(asset->rig.lookBone)] !=
            asset->skeleton.restPose().rotation[unsigned(asset->rig.lookBone)],
        "Look control must rotate head");
  Transform control;
  control.translation = {4, 3, -2};
  control.rotation = {0, .3f, 0, std::sqrt(.91f)};
  control.scale = {2, 2, 2};
  auto space = matrix(control);
  V3 pointIn{.2f, .5f, -.8f};
  check(distance(inversePoint(space, point(space, pointIn)), pointIn) < 1e-5f,
        "Control space round trip");
  MotionDatabase db;
  db.build(*asset);
  check(db.frames.size() >= 400,
        "Motion DB must contain actual sampled frames");
  for (const auto &frame : db.frames) {
    auto match = db.search(frame.feature, frame.tags, {}, false);
    check(match.cost == 0,
          "Exact database query must have zero feature distance");
  }
  Intent left;
  left.actualVelocity = left.desiredVelocity = {-3, 0, 0};
  left.speed = 3;
  auto match = db.search(db.query(*asset, asset->skeleton.restPose(), left), 1,
                         {}, false);
  check(asset->clips[match.clip].name == "walk_left",
        "Directional intent must select lateral movement");
  auto noTag = db.search({}, 0, {}, false);
  check(noTag.cost == std::numeric_limits<std::uint64_t>::max(),
        "Tags must filter all invalid candidates");
  Controller first, second;
  first.initialize(asset);
  second.initialize(asset);
  unsigned platformTick = 0;
  auto platform = [&](V3 p, float) {
    GroundHit hit;
    hit.hit = true;
    hit.surface = 1;
    hit.point = {p.x, .1f + platformTick * .0002f, p.z};
    hit.velocity = {0, .024f, 0};
    hit.surfaceToWorld[13] = hit.point.y;
    return hit;
  };
  first.setGroundQuery(platform);
  second.setGroundQuery(platform);
  float maxSlip = 0, maxPenetration = 0, maxSoft = 0, maxVolume = 0,
        maxMuscle = 0;
  for (unsigned tick = 1; tick <= 720; ++tick) {
    platformTick = tick;
    Intent in;
    in.tick = tick;
    in.position = {0, .1f + tick * .0002f, 0};
    in.hasLookTarget = true;
    in.lookTarget = {1, 1.5f, -2};
    if (tick > 180 && tick < 420) {
      in.speed = 3;
      in.actualVelocity = in.desiredVelocity = {-3, 0, 0};
      in.position.x = -float(tick - 180) * 3 / TickRate;
    }
    if (tick == 420)
      in.position.x = 10; // Explicit teleport resets constraints.
    in.cameraDistance = tick > 500 && tick < 600 ? 30.f : 2.f;
    if (tick == 200) {
      BoneMask mask;
      for (unsigned b = 4; b <= 9; ++b)
        mask.set(b);
      first.play("interact", mask);
      second.play("interact", mask);
    }
    first.tick(in);
    second.tick(in);
    check(hash(first) == hash(second),
          "Features must reproduce all hidden state");
    for (unsigned s = 0; s < 5; ++s)
      check(first.stages()[s].hash == second.stages()[s].hash,
            "Feature stage hash divergence");
    const auto &m = first.featureMetrics();
    if (tick > 30 && tick < 160) {
      maxSlip = std::max(maxSlip, m.footSlip);
      maxPenetration = std::max(maxPenetration, m.penetration);
      check(m.lockedFeet == 2, "Idle feet must stay locked on moving platform");
    }
    maxSoft = std::max(maxSoft, m.softMaxOffset);
    maxVolume = std::max(maxVolume, m.softVolumeError);
    maxMuscle = std::max(maxMuscle, m.muscleWeight);
    for (const auto &d : first.deformation())
      check(std::isfinite(d.position.x) && std::isfinite(d.normal.y),
            "Nonfinite render deformation");
    check(first.pose().translation[0] ==
              asset->skeleton.restPose().translation[0],
          "Features must not change locomotion root");
  }
  check(maxSlip < .05f, "Moving platform contact slip exceeded .05 m/s");
  check(maxPenetration < .002f,
        "Moving platform foot penetration exceeded 2 mm");
  check(maxSoft > 0 && maxSoft <= .0251f,
        "Soft tissue must move within the displacement budget");
  check(maxMuscle > .05f, "Muscle driver must produce nonzero weights");
  check(maxVolume < .01f,
        "Proxy volume error exceeded the frozen one percent fixture budget");
  check(first.decisions().size() == 720 && first.decisions().back().tick == 720,
        "Matching decisions must be inspectable");
  Controller slope;
  slope.initialize(asset);
  slope.setGroundQuery([](V3 p, float) {
    GroundHit h;
    h.hit = true;
    h.surface = 2;
    h.point = {p.x, .15f * p.x, p.z};
    float l = std::sqrt(1 + .15f * .15f);
    h.normal = {-.15f / l, 1 / l, 0};
    return h;
  });
  float slopeSlip = 0, slopePenetration = 0;
  for (unsigned tick = 1; tick <= 120; ++tick) {
    Intent in;
    in.tick = tick;
    slope.tick(in);
    if (tick > 60) {
      slopeSlip = std::max(slopeSlip, slope.featureMetrics().footSlip);
      slopePenetration =
          std::max(slopePenetration, slope.featureMetrics().penetration);
    }
  }
  check(slopeSlip < .05f && slopePenetration < .002f,
        "Slope contacts exceeded slip or penetration budget");
  // Disabled feature paths restore the undeformed reference and do not allocate
  // simulation points.
  Controller disabled;
  ControllerConfig config;
  config.proceduralRig = config.motionMatching = config.muscles =
      config.softTissue = false;
  disabled.initialize(asset, config);
  disabled.tick({1, 0, 0, 0, true});
  for (float w : disabled.featureState().morphWeights)
    check(w == 0, "Disabled muscle changed weights");
  for (const auto &d : disabled.deformation())
    check(distance(d.position, {}) == 0,
          "Disabled soft tissue changed vertices");
  // Save/restart must preserve physics, foot locks, matcher clocks and XPBD
  // velocities.
  engine::Scene scene;
  engine::Entity player;
  player.id = "actor";
  player.name = "actor";
  player.kind = "player";
  player.position = {0, 0, 0};
  player.scale = {.6f, 1.8f, .6f};
  engine::Entity floor;
  floor.id = "floor";
  floor.name = "floor";
  floor.kind = "ground";
  floor.position = {0, -.1f, 0};
  floor.scale = {30, .2f, 30};
  floor.collider = true;
  scene.entities = {player, floor};
  engine::Game game, resumed;
  game.initialize(scene);
  resumed.initialize(scene);
  game.setCharacter(asset);
  resumed.setCharacter(asset);
  engine::Input input;
  input.forward = .2f;
  for (unsigned i = 0; i < 90; ++i)
    game.update(input, 1.f / 120);
  auto saved = game.snapshot();
  resumed.restore(saved);
  for (unsigned i = 0; i < 90; ++i) {
    game.update(input, 1.f / 120);
    resumed.update(input, 1.f / 120);
    engine::determinism::Hasher a, b;
    game.digest(a);
    resumed.digest(b);
    check(a.value() == b.value(), "Feature snapshot continuation diverged");
  }
  auto invalid = game.snapshot();
  invalid["animation"]["features"]["soft"][0]["particles"][0]["position"]["x"] =
      std::numeric_limits<double>::infinity();
  try {
    game.restore(invalid);
    check(false, "Invalid feature snapshot accepted");
  } catch (const engine::gameplay::Error &) {
    check(true, "Invalid snapshot rejected");
  }
  // Foot phase machine on flat box ground: idle plants both feet, walking
  // alternates support without ever swinging both feet at once.
  auto flat = [](unsigned surface) {
    return [surface](V3 p, float) {
      GroundHit hit;
      hit.hit = true;
      hit.surface = surface;
      hit.point = {p.x, 0, p.z};
      hit.surfaceToWorld[13] = 0;
      return hit;
    };
  };
  Controller walk;
  walk.initialize(asset);
  walk.setGroundQuery(flat(7));
  for (unsigned tick = 1; tick <= 40; ++tick) {
    Intent in;
    in.tick = tick;
    walk.tick(in);
    check(walk.featureMetrics().lockedFeet == 2,
          "Idle feet must both support on flat ground");
    if (tick > 10)
      for (const auto &foot : walk.featureState().feet)
        check(foot.phase == FootPhase::Support,
              "Idle foot phase must settle on support");
  }
  unsigned swingTicks = 0, swingEvents = 0, supportTicks = 0;
  unsigned longestDoubleSwing = 0, doubleSwingRun = 0;
  float maxDrag = 0;
  bool previousContact[2] = {true, true};
  for (unsigned tick = 41; tick <= 280; ++tick) {
    Intent in;
    in.tick = tick;
    in.speed = 3;
    in.actualVelocity = in.desiredVelocity = {0, 0, -3};
    in.position = {0, 0, -float(tick - 40) * 3.f / TickRate};
    walk.tick(in);
    unsigned swinging = 0;
    for (const auto &foot : walk.featureState().feet)
      if ((foot.phase == FootPhase::Swing || foot.phase == FootPhase::Release) &&
          foot.weight > .5f)
        ++swinging;
    // The solver must not leave both feet airborne on its own account. A pin is
    // given up explicitly (drift, lift, overreach) when this fixture's authored
    // arc slides forward at sole height; that hand-over may overlap for a few
    // ticks, but it must stay short and every such tick must be accounted for by
    // a reported release reason.
    const auto &walkFeet = walk.featureState().feet;
    const bool bothAir = swinging > 1;
    if (bothAir) {
      doubleSwingRun = doubleSwingRun + 1;
      longestDoubleSwing = std::max(longestDoubleSwing, doubleSwingRun);
      if (tick > 80)
        check(walkFeet[0].release != ContactRelease::None ||
                  walkFeet[1].release != ContactRelease::None,
              "Both feet airborne without a reported release reason");
    } else
      doubleSwingRun = 0;
    swingTicks += swinging;
    // Release events are counted from the reported contact so the check also
    // covers content that never gets pinned.
    for (unsigned side = 0; side < 2; ++side) {
      const bool now = walk.featureState().feet[side].contact;
      if (previousContact[side] && !now)
        ++swingEvents;
      previousContact[side] = now;
    }
    // Reported contact keeps alternating from the clip's windows. This fixture's
    // authored foot arcs hover well above the sole plane, so the geometric plant
    // test correctly refuses to anchor them: pinning a foot the clip has already
    // lifted is what drags the leg behind the body.
    for (const auto &foot : walk.featureState().feet) {
      if (foot.contact && tick > 80)
        ++supportTicks;
      if (!foot.locked)
        continue;
      const float dx = foot.target.x - foot.animated.x;
      const float dz = foot.target.z - foot.animated.z;
      maxDrag = std::max(maxDrag, std::sqrt(dx * dx + dz * dz));
    }
  }
  check(supportTicks > 60, "Walking must establish alternating support windows");
  check(longestDoubleSwing <= 8,
        "A hand-over must not leave both feet airborne for long");
  check(swingTicks > 0 && swingEvents > 0,
        "Walking must release and land feet");
  // A pin may bend the leg toward its anchor, but never drag it away from the
  // pose the clip draws: that is exactly the "leg stuck behind the body" bug.
  check(maxDrag <= .32f,
        "A planted foot must not drag the leg away from the clip");
  check(walk.featureMetrics().groundQueries > 0,
        "Ground query budget must be counted");
  check(walk.featureState().fallback != RigFallback::Disabled,
        "Flat walking must keep the rig active");

  // A static idle clip must not freeze the rig: the procedural idle layer adds
  // breathing and a weight shift, while the foot solve keeps both soles on the
  // rest sole plane instead of flipping them around the foot bone's local axes.
  auto idle = std::make_unique<Controller>();
  idle->initialize(asset);
  idle->setGroundQuery(flat(13));
  check(asset->clips[asset->clipIndex("idle")].staticPose(),
        "Idle fixture must exercise the static-idle fallback");
  V3 idlePelvis{};
  for (unsigned tick = 1; tick <= 180; ++tick) {
    Intent in;
    in.tick = tick;
    idle->tick(in);
    if (tick != 90 && tick != 180)
      continue;
    const auto &pose = idle->pose();
    const auto globals = asset->skeleton.globals(pose);
    const auto restGlobals = asset->skeleton.globals(asset->skeleton.restPose());
    for (unsigned side = 0; side < asset->rig.legs.size(); ++side) {
      const auto tip = asset->rig.legs[side].tip;
      float alignment = 0;
      for (unsigned c = 0; c < 3; ++c)
        for (unsigned r = 0; r < 3; ++r)
          alignment += globals[tip][c * 4 + r] * restGlobals[tip][c * 4 + r];
      check(alignment > 2.99f,
            "Idle foot must stay on the rest sole plane");
      check(std::abs(globals[tip][13] -
                     asset->rig.legs[side].soleHeight) < .01f,
            "Idle ankle must sit at the rig sole height");
    }
    const unsigned pelvis = unsigned(asset->rig.pelvis);
    const V3 pelvisNow{pose.translation[pelvis][0], pose.translation[pelvis][1],
                       pose.translation[pelvis][2]};
    if (tick == 90)
      idlePelvis = pelvisNow;
    else
      check(distance(idlePelvis, pelvisNow) > .002f,
            "Static idle clip must still produce visible idle motion");
  }

  // In-place turning: body heading advances while the support foot stays
  // planted, then one foot steps along an arc while the other supports.
  Controller turn, turnCopy;
  turn.initialize(asset);
  turnCopy.initialize(asset);
  turn.setGroundQuery(flat(9));
  turnCopy.setGroundQuery(flat(9));
  for (unsigned tick = 1; tick <= 60; ++tick) {
    Intent in;
    in.tick = tick;
    turn.tick(in);
    turnCopy.tick(in);
  }
  check(turn.featureMetrics().lockedFeet == 2,
        "Turn start needs both feet planted");
  V3 anchorBefore[2] = {turn.featureState().feet[0].anchor,
                        turn.featureState().feet[1].anchor};
  // Slip and intentional rotation are measured separately: a turn is supposed to
  // rotate the sole on the spot, and only the part that is not asked for counts
  // against contact quality.
  float turnSlip = 0, turnPivot = 0;
  float heading = 0;
  for (unsigned tick = 61; tick <= 520; ++tick) {
    heading += .02f;
    Intent in;
    in.tick = tick;
    in.heading = heading;
    in.angularVelocity = 2.4f;
    turn.tick(in);
    turnCopy.tick(in);
    const auto &feet = turn.featureState().feet;
    turnSlip = std::max(turnSlip, turn.featureMetrics().contactSlip);
    turnPivot = std::max(turnPivot, turn.featureMetrics().pivotSlip);
    unsigned swinging = 0;
    for (const auto &foot : feet)
      if (foot.phase == FootPhase::Swing && foot.weight > .5f)
        ++swinging;
    check(swinging <= 1, "Turn stepping must keep one support foot");
    for (unsigned s = 0; s < 2; ++s)
      if (feet[s].turnStep) {
        check(feet[1 - s].phase == FootPhase::Support &&
                  feet[1 - s].locked,
              "The other foot must support a turn step");
      }
    if (tick % 60 == 0)
      check(hash(turn) == hash(turnCopy),
            "Turn stepping must stay deterministic");
  }
  check(turn.featureState().turnSteps >= 2,
        "Continuous in-place turning must produce alternating steps");
  check(std::isfinite(turn.featureMetrics().turnResidual),
        "Turn residual must stay finite");
  // Measured 0.00 m/s of unintended slide and 0.41 m/s of requested pivot on the
  // bundled character. The pivot is the foot being turned on the spot, which is
  // what the residual asks for; it must not leak into the slip metric.
  check(turnSlip < .05f,
        "In-place turning must not slide the support sole");
  check(turnPivot > 0, "Turning must still rotate the support sole on purpose");
  const bool anchorMoved =
      distance(turn.featureState().feet[0].anchor, anchorBefore[0]) > .01f ||
      distance(turn.featureState().feet[1].anchor, anchorBefore[1]) > .01f;
  check(anchorMoved, "Turn stepping must re-plant at least one foot");

  // Legacy clips without contact metadata stay conservative: no anchor while
  // moving, planted idle when standing still.
  auto legacy = std::make_shared<Asset>(*asset);
  for (auto &clip : legacy->clips) {
    clip.contactSource = Clip::ContactSource::Unknown;
    clip.contactConfidence = 0;
  }
  Controller conservative;
  conservative.initialize(legacy);
  conservative.setGroundQuery(flat(3));
  for (unsigned tick = 1; tick <= 60; ++tick) {
    Intent in;
    in.tick = tick;
    in.speed = 3;
    in.actualVelocity = in.desiredVelocity = {0, 0, -3};
    in.position = {0, 0, -float(tick) * 3.f / TickRate};
    conservative.tick(in);
    check(conservative.featureMetrics().lockedFeet == 0,
          "Unknown contacts must not lock feet while moving");
  }
  // The idle state transition releases the feet for the blend window, then the
  // conservative fallback must plant them.
  for (unsigned tick = 61; tick <= 130; ++tick) {
    Intent in;
    in.tick = tick;
    conservative.tick(in);
  }
  check(conservative.featureMetrics().lockedFeet == 2,
        "Unknown contacts must fall back to a planted idle");

  // Model-space rig sizes must be scaled into world space.
  Controller scaled;
  scaled.initialize(asset);
  scaled.setGroundQuery(flat(11));
  for (unsigned tick = 1; tick <= 120; ++tick) {
    Intent in;
    in.tick = tick;
    in.modelScale = 2.f;
    scaled.tick(in);
  }
  check(scaled.featureMetrics().lockedFeet == 2,
        "Scaled character must still plant both feet");
  check(scaled.featureMetrics().penetration < .01f,
        "Scaled character sole penetration budget");
  check(std::abs(scaled.featureState().feet[0].target.y -
                 asset->rig.legs[0].soleHeight * 2.f) < .05f,
        "Scaled foot target must apply the model scale");

  // The game installs its ground query before resetting the controller, so the
  // reset evaluation locks feet with the default model scale. The first tick
  // must re-anchor them at the instance scale instead of keeping them buried.
  auto scaledReset = std::make_unique<Controller>();
  scaledReset->setGroundQuery(flat(12));
  scaledReset->initialize(asset);
  for (unsigned tick = 1; tick <= 30; ++tick) {
    Intent in;
    in.tick = tick;
    in.modelScale = 2.f;
    scaledReset->tick(in);
  }
  check(std::abs(scaledReset->featureState().feet[0].target.y -
                 asset->rig.legs[0].soleHeight * 2.f) < .02f,
        "Reset must re-anchor feet at the instance model scale");

  // Scene instances may override the asset rig defaults.
  engine::Scene overrideScene;
  engine::Entity overridePlayer;
  overridePlayer.id = "override-player";
  overridePlayer.name = "override player";
  overridePlayer.kind = "player";
  overridePlayer.position = {0, 0, 0};
  overridePlayer.scale = {.6f, 1.8f, .6f};
  overridePlayer.character.rig = {{"turnInPlace", false},
                                  {"turnCooldown", .5},
                                  {"maxSlopeCosine", .5}};
  engine::Entity overrideFloor;
  overrideFloor.id = "override-floor";
  overrideFloor.name = "override floor";
  overrideFloor.kind = "ground";
  overrideFloor.position = {0, -.1f, 0};
  overrideFloor.scale = {30, .2f, 30};
  overrideFloor.collider = true;
  overrideScene.entities = {overridePlayer, overrideFloor};
  engine::Game overrideGame;
  overrideGame.initialize(overrideScene);
  overrideGame.setCharacter(asset);
  check(!overrideGame.animationController().rigSettings().turnInPlace,
        "Scene rig override must disable turn stepping");
  check(std::abs(overrideGame.animationController().rigSettings().turnCooldown -
                 .5f) < 1e-5f,
        "Scene rig override must apply parameters");

  // The bundled Mixamo-style robot is the real-asset regression for foot bones
  // whose local +Y points along the toes instead of up.
  if (std::filesystem::exists("assets/builtin/robot/character.json")) {
    auto robot = loadAsset("assets/builtin/robot/character.json");
    check(robot->clips[robot->clipIndex("idle")].staticPose(),
          "Bundled idle clip must be static");
    auto builtin = std::make_unique<Controller>();
    builtin->setGroundQuery(flat(21));
    ControllerConfig builtinConfig;
    builtinConfig.clips = robot->controllerClips;
    builtin->initialize(robot, builtinConfig);
    for (unsigned tick = 1; tick <= 60; ++tick) {
      Intent in;
      in.tick = tick;
      in.modelScale = 2.f;
      builtin->tick(in);
    }
    const auto globals = robot->skeleton.globals(builtin->pose());
    const auto restGlobals =
        robot->skeleton.globals(robot->skeleton.restPose());
    for (unsigned side = 0; side < robot->rig.legs.size(); ++side) {
      const auto tip = robot->rig.legs[side].tip;
      float alignment = 0;
      for (unsigned c = 0; c < 3; ++c)
        for (unsigned r = 0; r < 3; ++r)
          alignment += globals[tip][c * 4 + r] * restGlobals[tip][c * 4 + r];
      check(alignment > 2.99f,
            "Bundled foot must keep its rest sole plane");
      check(std::abs(globals[tip][13] -
                     robot->rig.legs[side].soleHeight) < .01f,
            "Bundled foot must sit at the rig sole height");
    }
  }

  // Contact-driven motion on the bundled character used by the sample project.
  // Single support must hold the sole in place while the other foot swings, the
  // pelvis compensation must stay inside its budget, and the toe-off roll must
  // move the ankle without moving the contact edge.
  float walkSlip = 0, walkAnchor = 0, walkSoleAngle = 0, walkJump = 0,
        walkShift = 0, walkPenetration = 0, walkIk = 0;
  unsigned walkSupportTicks = 0, walkTicks = 0, walkToeOffs = 0,
           walkLifts = 0, walkStrides = 0, walkSwitches = 0;
  if (std::filesystem::exists("assets/builtin/robot/character.json")) {
    auto robot = loadAsset("assets/builtin/robot/character.json");
    ControllerConfig gait;
    gait.clips = robot->controllerClips;
    const float scale = 1.84f;
    const float shiftLimit = gait.rigOverride ? gait.rig.maxPelvisShift
                                              : robot->rig.settings.maxPelvisShift;
    const float rollLimit = robot->rig.settings.soleRoll;
    Controller walker;
    walker.setGroundQuery(flat(31));
    walker.initialize(robot, gait);
    for (unsigned tick = 1; tick <= 60; ++tick) {
      Intent in;
      in.tick = tick;
      in.modelScale = scale;
      walker.tick(in);
    }
    check(walker.featureMetrics().lockedFeet == 2,
          "Bundled idle must plant both feet");
    check(walker.featureMetrics().contactSlip < .02f &&
              walker.featureMetrics().anchorError < .005f,
          "Bundled idle sole must not slide");
    check(walker.featureMetrics().pelvisShiftUsed < 1e-3f,
          "Standing must not need horizontal pelvis travel");
    const float startZ = 0;
    for (unsigned tick = 61; tick <= 300; ++tick) {
      Intent in;
      in.tick = tick;
      in.speed = 3.f;
      in.actualVelocity = in.desiredVelocity = {0, 0, -3.f};
      in.position = {0, 0, startZ - float(tick - 60) * 3.f / TickRate};
      in.modelScale = scale;
      walker.tick(in);
      const auto &m = walker.featureMetrics();
      ++walkTicks;
      walkSlip = std::max(walkSlip, m.contactSlip);
      walkAnchor = std::max(walkAnchor, m.anchorError);
      walkSoleAngle = std::max(walkSoleAngle, m.soleAngle);
      walkJump = std::max(walkJump, m.switchJump);
      walkShift = std::max(walkShift, m.pelvisShiftUsed);
      walkPenetration = std::max(walkPenetration, m.penetration);
      walkIk = std::max(walkIk, m.ikError);
      walkToeOffs += m.toeOffs;
      walkLifts += m.releaseCounts[std::size_t(ContactRelease::Lift)];
      walkStrides += m.releaseCounts[std::size_t(ContactRelease::Stride)];
      walkSwitches += m.supportSwitches;
      if (m.lockedFeet >= 1)
        ++walkSupportTicks;
    }
    // The bundled walk covers 64% of its cycle with authored contact windows, so
    // a support foot is expected for most ticks; the rest is the clip's own
    // uncovered phase, decided by geometry. The first 30 ticks are the idle to
    // walk blend, which deliberately carries both feet before the solve takes
    // over, so the ratio is measured over the whole run with room for it.
    check(walkSupportTicks >= walkTicks * 5 / 6,
          "Walking must keep a planted foot for almost every tick");
    check(walkSwitches >= 4, "Walking must hand the support over between feet");
    // Measured 0.13 m/s after the stance budget landed: the contact now ends a
    // tick before the leg over-extends, the sole pitch gives way when the ankle
    // cannot afford it, and the hips follow a stretched leg instead of averaging
    // the two. Before that the same content reported 1.05 m/s, nearly all of it
    // in the two ticks where the toe-off roll pushed the ankle past full reach
    // and the IK clamp dragged the contact edge along.
    check(walkSlip < .2f, "Sole contact slip exceeded the walk budget");
    check(walkAnchor < .01f, "Locked ankle residual exceeded the walk budget");
    check(walkSoleAngle <= rollLimit + .02f,
          "Sole pitch must stay inside the toe-off roll limit");
    check(walkJump < .2f, "Support hand-over must not pop the ankle");
    check(walkShift <= shiftLimit * scale + 1e-3f,
          "Horizontal pelvis compensation must stay inside its budget");
    check(walkToeOffs > 0, "Gait must roll over the toe at the end of stance");
    check(walkPenetration < .03f, "Bundled gait must not sink the shoe");
    // A stance can end two ways: the clip lifts the foot off the surface, or the
    // stride runs out of leg first. The second is the normal ending on this
    // content now that the contact is given up before the IK has to clamp.
    check(walkLifts + walkStrides > 0,
          "Gait must release support at the end of a stance");
    // Snapshot restore keeps the new contact state; a save written before the
    // constraint split must still load instead of failing the restore.
    engine::Scene snapScene;
    engine::Entity snapPlayer;
    snapPlayer.id = "rig-player";
    snapPlayer.name = "rig player";
    snapPlayer.kind = "player";
    snapPlayer.position = {0, 0, 0};
    snapPlayer.scale = {.6f, 1.8f, .6f};
    engine::Entity snapFloor;
    snapFloor.id = "rig-floor";
    snapFloor.name = "rig floor";
    snapFloor.kind = "ground";
    snapFloor.position = {0, -.1f, 0};
    snapFloor.scale = {30, .2f, 30};
    snapFloor.collider = true;
    snapScene.entities = {snapPlayer, snapFloor};
    // The snapshot round trip needs two full game instances. They live in their
    // own scope so the test frame does not carry four controllers at once.
    {
      engine::Scene snapScene;
      engine::Entity snapPlayer;
      snapPlayer.id = "rig-player";
      snapPlayer.name = "rig player";
      snapPlayer.kind = "player";
      snapPlayer.position = {0, 0, 0};
      snapPlayer.scale = {.6f, 1.8f, .6f};
      engine::Entity snapFloor;
      snapFloor.id = "rig-floor";
      snapFloor.name = "rig floor";
      snapFloor.kind = "ground";
      snapFloor.position = {0, -.1f, 0};
      snapFloor.scale = {30, .2f, 30};
      snapFloor.collider = true;
      snapScene.entities = {snapPlayer, snapFloor};
      auto snapGame = std::make_unique<engine::Game>();
      auto snapResumed = std::make_unique<engine::Game>();
      snapGame->initialize(snapScene);
      snapResumed->initialize(snapScene);
      snapGame->setCharacter(robot);
      snapResumed->setCharacter(robot);
      engine::Input snapInput;
      snapInput.forward = .6f;
      for (unsigned i = 0; i < 120; ++i)
        snapGame->update(snapInput, 1.f / 120);
      auto saved = snapGame->snapshot();
      check(saved.at("animation").at("features").contains("pelvisShift"),
            "Snapshot must carry the horizontal pelvis compensation");
      snapResumed->restore(saved);
      for (unsigned i = 0; i < 60; ++i) {
        snapGame->update(snapInput, 1.f / 120);
        snapResumed->update(snapInput, 1.f / 120);
        engine::determinism::Hasher a, b;
        snapGame->digest(a);
        snapResumed->digest(b);
        check(a.value() == b.value(),
              "Restored contact state must continue deterministically");
      }
      auto legacySave = saved;
      legacySave["animation"]["features"].erase("pelvisShift");
      for (auto &foot : legacySave["animation"]["features"]["feet"]) {
        foot.erase("mode");
        foot.erase("release");
        foot.erase("roll");
        foot.erase("pivot");
        foot.erase("previousPivot");
        foot.erase("pivotValid");
        foot.erase("pivoting");
      }
      legacySave["animation"]["config_"]["rig"].erase("maxPelvisShift");
      legacySave["animation"]["config_"]["rig"].erase("soleRoll");
      bool restored = true;
      try {
        snapResumed->restore(legacySave);
      } catch (const engine::gameplay::Error &) {
        restored = false;
      }
      check(restored,
            "A snapshot without the contact fields must still restore");
      // An older save cannot reproduce the contact details it never stored, so
      // only the restore itself is required here; the new fields re-derive from
      // the clip phase on the following ticks.
      engine::Input resumeInput;
      resumeInput.forward = .6f;
      snapResumed->update(resumeInput, 1.f / 120);
    }
    // A teleport is an explicit release, not an unexplained one.
    Controller teleport;
    teleport.setGroundQuery(flat(32));
    teleport.initialize(robot, gait);
    for (unsigned tick = 1; tick <= 60; ++tick) {
      Intent in;
      in.tick = tick;
      in.modelScale = scale;
      teleport.tick(in);
    }
    Intent jump;
    jump.tick = 61;
    jump.modelScale = scale;
    jump.position = {6, 0, -4};
    teleport.tick(jump);
    check(teleport.featureMetrics()
                  .releaseCounts[std::size_t(ContactRelease::Teleport)] == 2,
          "Teleport must report both contacts as teleport releases");
  }

  // Generic contacts (multi-contact stage): a hand holds a rail while the body
  // turns, keeps holding until the arm runs out, and lets go with a reason on
  // teleport, on expiry and when the body walks away.
  if (std::filesystem::exists("assets/builtin/robot/character.json")) {
    auto robot = loadAsset("assets/builtin/robot/character.json");
    auto findBone = [](const Skeleton &s, const char *needle) {
      for (unsigned i = 0; i < s.bones.size(); ++i)
        if (s.bones[i].name.find(needle) != std::string::npos)
          return int(i);
      return -1;
    };
    const int shoulder = findBone(robot->skeleton, "LeftArm");
    const int elbow = findBone(robot->skeleton, "LeftForeArm");
    const int wrist = findBone(robot->skeleton, "LeftHand");
    check(shoulder >= 0 && elbow >= 0 && wrist >= 0,
          "Bundled character must expose a left arm chain");
    const float scale = 1.84f;
    ControllerConfig gait;
    gait.clips = robot->controllerClips;
    // Heap allocated on purpose: this function is close to the one megabyte
    // stack the test runner gets, and each Controller is ~54 KB of pose buffers.
    auto holder = std::make_unique<Controller>();
    auto holderCopy = std::make_unique<Controller>();
    holder->setGroundQuery(flat(41));
    holderCopy->setGroundQuery(flat(41));
    holder->initialize(robot, gait);
    holderCopy->initialize(robot, gait);
    RigChain arm;
    arm.root = unsigned(shoulder);
    arm.joint = unsigned(elbow);
    arm.tip = unsigned(wrist);
    arm.pole = {.25f, -.2f, .3f};
    const int chain = int(holder->addContactChain(arm));
    holderCopy->addContactChain(arm);
    check(holder->contactChains().size() == 1,
          "Contact chain registration must be recorded");
    for (unsigned tick = 1; tick <= 60; ++tick) {
      Intent in;
      in.tick = tick;
      in.modelScale = scale;
      holder->tick(in);
      holderCopy->tick(in);
    }
    const auto restGlobals = robot->skeleton.globals(holder->pose());
    // A rail is taken with a bent elbow: aim inside the reach so the body has
    // room to move while the hand stays where it was put.
    auto aim = [&](float k) {
      return V3{(restGlobals[unsigned(shoulder)][12] +
                 (restGlobals[unsigned(wrist)][12] -
                  restGlobals[unsigned(shoulder)][12]) * k) * scale,
                (restGlobals[unsigned(shoulder)][13] +
                 (restGlobals[unsigned(wrist)][13] -
                  restGlobals[unsigned(shoulder)][13]) * k) * scale,
                (restGlobals[unsigned(shoulder)][14] +
                 (restGlobals[unsigned(wrist)][14] -
                  restGlobals[unsigned(shoulder)][14]) * k) * scale};
    };
    const V3 grab = aim(.8f);
    check(holder->setContact(1, chain, grab, {0, 1, 0}, .8f, 0) &&
              holderCopy->setContact(1, chain, grab, {0, 1, 0}, .8f, 0),
          "A registered chain must accept a hold");
    float holdError = 0;
    unsigned gripTicks = 0, heldTicks = 0;
    for (unsigned tick = 61; tick <= 200; ++tick) {
      const float heading = .698f * std::min(1.f, float(tick - 60) / 120.f);
      Intent in;
      in.tick = tick;
      in.modelScale = scale;
      in.heading = heading;
      in.turnTarget = heading;
      in.hasTurnTarget = true;
      holder->tick(in);
      holderCopy->tick(in);
      check(hash(*holder) == hash(*holderCopy),
            "A held contact must stay deterministic");
      ++gripTicks;
      const auto &m = holder->featureMetrics();
      if (m.activeContacts)
        ++heldTicks;
      holdError = std::max(holdError, m.contactError);
      check(m.lockedFeet >= 1, "Holding a rail must not lift both feet");
    }
    check(heldTicks == gripTicks,
          "A hand must keep its hold while the body turns");
    // Measured 0.00 m once the weight ramp settles: the palm sits on the rail.
    check(holdError < .02f, "A held hand must not drift off its contact");
    // Walking away from the rail has to end the hold with a reason, and the
    // entry must not linger in the state.
    unsigned overreach = 0;
    for (unsigned tick = 201; tick <= 320; ++tick) {
      Intent in;
      in.tick = tick;
      in.modelScale = scale;
      in.speed = 3.f;
      in.actualVelocity = in.desiredVelocity = {0, 0, -3.f};
      in.position = {0, 0, -float(tick - 200) * 3.f / TickRate};
      holder->tick(in);
      overreach += holder->featureMetrics()
                       .releaseCounts[std::size_t(ContactRelease::Overreach)];
    }
    check(overreach > 0, "Walking away from a hold must release it");
    check(holder->contacts().empty(), "A released hold must not linger");
    holder.reset();

    // A hold that is allowed to slip: the palm travels along a wall while the
    // body moves. It still drives the arm, but it must not pull the hips and it
    // must not report its own slide as tracking error.
    auto slider = std::make_unique<Controller>();
    slider->setGroundQuery(flat(44));
    slider->initialize(robot, gait);
    slider->addContactChain(arm);
    for (unsigned tick = 1; tick <= 30; ++tick) {
      Intent in;
      in.tick = tick;
      in.modelScale = scale;
      slider->tick(in);
    }
    check(slider->setContact(5, 0, grab, {0, 1, 0}, .5f, 0, true),
          "A sliding hold must be accepted");
    float slipError = 0, slipSpeed = 0;
    unsigned slidingTicks = 0;
    for (unsigned tick = 31; tick <= 150; ++tick) {
      Intent in;
      in.tick = tick;
      in.modelScale = scale;
      in.speed = 1.2f;
      in.actualVelocity = in.desiredVelocity = {0, 0, -1.2f};
      in.position = {0, 0, -float(tick - 30) * 1.2f / TickRate};
      slider->tick(in);
      const auto &m = slider->featureMetrics();
      slidingTicks += m.activeContacts ? 1 : 0;
      slipError = std::max(slipError, m.contactError);
      slipSpeed = std::max(slipSpeed, m.holdSlip);
    }
    check(slidingTicks > 100,
          "A sliding hold must stay active while the body moves");
    check(slipSpeed < 1e-6f,
          "A sliding hold must not report its own slide as slip");
    check(slipError < .05f, "A sliding hold must track the hand it follows");
    slider.reset();

    // A validity window ends the hold by itself, and a teleport drops it.
    auto timed = std::make_unique<Controller>();
    timed->setGroundQuery(flat(42));
    timed->initialize(robot, gait);
    timed->addContactChain(arm);
    for (unsigned tick = 1; tick <= 30; ++tick) {
      Intent in;
      in.tick = tick;
      in.modelScale = scale;
      timed->tick(in);
    }
    check(timed->setContact(9, 0, grab, {0, 1, 0}, .5f, 45),
          "A hold with an expiry must be accepted");
    unsigned expired = 0;
    for (unsigned tick = 31; tick <= 60; ++tick) {
      Intent in;
      in.tick = tick;
      in.modelScale = scale;
      timed->tick(in);
      expired += timed->featureMetrics()
                     .releaseCounts[std::size_t(ContactRelease::Expired)];
    }
    check(expired > 0, "An expiring hold must report the expiry");
    check(timed->contacts().empty(), "An expired hold must not linger");
    timed.reset();
    auto jump = std::make_unique<Controller>();
    jump->setGroundQuery(flat(43));
    jump->initialize(robot, gait);
    jump->addContactChain(arm);
    for (unsigned tick = 1; tick <= 30; ++tick) {
      Intent in;
      in.tick = tick;
      in.modelScale = scale;
      jump->tick(in);
    }
    check(jump->setContact(3, 0, grab, {0, 1, 0}, .5f, 0),
          "A hold must be accepted before the teleport");
    for (unsigned tick = 31; tick <= 40; ++tick) {
      Intent in;
      in.tick = tick;
      in.modelScale = scale;
      jump->tick(in);
    }
    Intent warp;
    warp.tick = 41;
    warp.modelScale = scale;
    warp.position = {6, 0, -4};
    jump->tick(warp);
    check(jump->featureMetrics()
                  .releaseCounts[std::size_t(ContactRelease::Teleport)] == 3,
          "A teleport must release the feet and the hold");
    check(jump->contacts().empty(), "A teleport must drop every hold");
  }

  // Locomotion clips carry a measured support speed. Playback must follow the
  // character's real speed (and the instance scale) instead of always running
  // at 1x, which made the legs trail the body at higher scene speeds.
  auto played = std::make_shared<Asset>(*asset);
  played->clips[played->clipIndex("walk")].locomotionSpeed = 2.f;
  played->clips[played->clipIndex("run")].locomotionSpeed = 2.f;
  ControllerConfig replayedConfig;
  replayedConfig.clips = played->controllerClips;
  replayedConfig.walkSpeed = 3.f;
  replayedConfig.runSpeed = 6.f;
  // Re-timing applies to the clip-driven path. The matcher owns its own frame
  // clock, so it deliberately plays at 1x and is exercised separately.
  replayedConfig.motionMatching = false;
  auto replayedPtr = std::make_unique<Controller>();
  replayedPtr->initialize(played, replayedConfig);
  replayedPtr->setGroundQuery(flat(15));
  for (unsigned tick = 1; tick <= 24; ++tick) {
    Intent in;
    in.tick = tick;
    in.speed = 3;
    in.actualVelocity = in.desiredVelocity = {0, 0, -3};
    in.position = {0, 0, -float(tick) * 3.f / TickRate};
    in.modelScale = tick > 16 ? 2.f : 1.f;
    replayedPtr->tick(in);
  }
  check(std::abs(replayedPtr->playbackRate() - .75f) < 1e-3f,
        "Playback must follow measured support speed and model scale");
  auto unscaled = std::make_unique<Controller>();
  unscaled->initialize(played, replayedConfig);
  unscaled->setGroundQuery(flat(16));
  for (unsigned tick = 1; tick <= 24; ++tick) {
    Intent in;
    in.tick = tick;
    in.speed = 3;
    in.actualVelocity = in.desiredVelocity = {0, 0, -3};
    in.position = {0, 0, -float(tick) * 3.f / TickRate};
    unscaled->tick(in);
  }
  check(std::abs(unscaled->playbackRate() - 1.5f) < 1e-3f,
        "Unscaled playback must be speed over clip support speed");

  return {{"checks", checks},
          {"databaseFrames", db.frames.size()},
          {"movingPlatformSlipMps", maxSlip},
          {"footPenetrationM", maxPenetration},
          {"slopeSlipMps", slopeSlip},
          {"slopePenetrationM", slopePenetration},
          {"softMaxOffsetM", maxSoft},
          {"softMaxVolumeError", maxVolume},
          {"muscleMaxWeight", maxMuscle},
          {"walkContactSlipMps", walkSlip},
          {"walkAnchorErrorM", walkAnchor},
          {"walkSoleAngleRad", walkSoleAngle},
          {"walkSwitchJumpM", walkJump},
          {"walkPelvisShiftM", walkShift},
          {"walkSupportTicks", walkSupportTicks},
          {"walkToeOffs", walkToeOffs},
          {"ticksCompared", 720},
          {"snapshotContinuationTicks", 90}};
}
