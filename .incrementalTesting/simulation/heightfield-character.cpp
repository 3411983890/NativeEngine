#include "../../engine/native/game.hpp"
#include "../../host/scene_loader.hpp"
#include <fstream>
#include <cmath>
#include <iostream>
#include <stdexcept>

using namespace engine;
namespace {
unsigned checks = 0;
void check(bool ok, const char* message) {
    ++checks;
    if (!ok) throw std::runtime_error(message);
}
Entity object(const char* id, const char* kind, Vec3 position, Vec3 scale) {
    Entity e; e.id = e.name = id; e.kind = kind; e.position = position;
    e.scale = scale; e.collider = true; return e;
}
Scene hillside(float slope) {
    Scene scene;
    auto terrain = object("terrain", "ground", {0, 4, 0}, {40, 12, 40});
    terrain.hasHeightfield = terrain.heightfieldEnabled = true;
    terrain.heightfieldOrigin[0] = terrain.heightfieldOrigin[1] = -20;
    terrain.heightfieldSize[0] = terrain.heightfieldSize[1] = 40;
    terrain.heightfieldWidth = terrain.heightfieldHeight = 41;
    for (int z = 0; z <= 40; ++z) for (int x = 0; x <= 40; ++x)
        terrain.heightfieldHeights.push_back(4 + slope * (z - 20) + .08f * std::sin(float(x - 20)));
    auto player = object("actor", "player", {0, 4, 0}, {.6f, 1.8f, .6f});
    player.character.scale = {1.8406537f, 1.8406537f, 1.8406537f};
    scene.entities = {terrain, player};
    return scene;
}
void settle(Game& game) { for (int t = 0; t < 120; ++t) game.update({}, 1.f / 120); }
void walkSlope(float slope, const std::shared_ptr<const animation::Asset>& asset) {
    Game game; game.initialize(hillside(slope)); game.setCharacter(asset, true); settle(game);
    unsigned supported = 0;
    Input move; move.forward = 1;
    for (int t = 0; t < 180; ++t) {
        game.update(move, 1.f / 120);
        supported += game.grounded();
        check(game.animationController().featureState().fallback != animation::RigFallback::NoGround,
              "Walking CDM lost its heightfield ground query");
        const auto p = game.playerPosition();
        check(std::abs(p.y - (4 + slope * p.z)) < .025f, "Slope walking floated above or sank below the actual terrain");
    }
    check(supported >= 178, "Downhill walking repeatedly lost ground support");
    const float takeoff = game.playerPosition().y;
    move.jump = true; game.update(move, 1.f / 120);
    check(!game.grounded(), "Walking jump did not leave terrain");
    for (const auto& foot : game.animationController().featureState().feet)
        check(!foot.locked, "CDM retained a support foot during takeoff");
    move.jump = false;
    float peak = game.playerPosition().y;
    for (int t = 0; t < 90; ++t) {
        game.update(move, 1.f / 120); peak = std::max(peak, game.playerPosition().y);
    }
    check(peak > takeoff + .5f, "Slope/step grace cancelled the jump velocity");
}
}
int main(int argc, char** argv) {
    try {
        check(argc >= 2 && argc <= 4, "Expected builtin character header and optional project scene/report");
        const auto asset = animation::loadAsset(argv[1]);
        // The terrain's box proxy top is 10 m, while its actual surface is near 4 m.
        // CDM must hit the heightfield, never the proxy or a clamped point outside it.
        Game game; game.initialize(hillside(.22f)); game.setCharacter(asset, true); settle(game);
        unsigned contactTicks = 0;
        for (int t = 0; t < 120; ++t) {
            game.update({}, 1.f / 120);
            const auto& controller = game.animationController();
            check(controller.featureState().fallback != animation::RigFallback::NoGround,
                  "CDM queried the terrain box proxy instead of its height samples");
            check(controller.featureState().fallback != animation::RigFallback::NotGrounded,
                  "Idle character on terrain was reported airborne");
            for (const auto& foot : controller.featureState().feet) if (foot.contact) {
                ++contactTicks;
                check(foot.normal.y < .995f && foot.normal.y > .9f,
                      "CDM did not receive the hillside surface normal");
            }
        }
        check(contactTicks > 30, "CDM never established support on uneven terrain");
        walkSlope(.3f, asset); walkSlope(-.3f, asset); walkSlope(0, asset);
        // Following a gentle slope must not turn a steep drop into glued feet.
        auto ledge = hillside(0);
        for (int z = 0; z <= 40; ++z) for (int x = 0; x <= 40; ++x)
            ledge.entities[0].heightfieldHeights[z * 41 + x] = z < 20 ? 2.f : 4.f;
        ledge.entities[1].position = {0, 4, 1};
        game.initialize(ledge); settle(game);
        Input descend; descend.forward = 1;
        unsigned falling = 0;
        for (int t = 0; t < 100; ++t) {
            game.update(descend, 1.f/120);
            if (game.playerPosition().z < -.1f && game.playerPosition().z > -.9f)
                falling += !game.grounded();
        }
        check(falling > 5, "Slope support incorrectly snapped the actor down a steep drop");
        auto platform = hillside(.22f);
        platform.entities.insert(platform.entities.begin(), object("platform", "prop", {0,4.25f,0}, {4,.3f,4}));
        platform.entities.back().position.y = 4.4f;
        game.initialize(platform); game.setCharacter(asset, true); settle(game);
        for (const auto& foot : game.animationController().featureState().feet)
            check(foot.contact && foot.normal.y > .999f, "Terrain query overrode a nearer box support surface");
        // Jump during the forward slide onto a box tread, while step grace is live.
        Scene stairs;
        stairs.entities = {object("floor", "ground", {0,-.5f,0}, {40,1,40}),
            object("actor", "player", {0,0,1}, {.6f,1.8f,.6f}),
            object("step", "prop", {0,.1f,0}, {4,.2f,1})};
        game.initialize(stairs); Input move; move.forward = 1;
        for (int t = 0; t < 120 && game.playerPosition().y < .19f; ++t) game.update(move, 1.f/120);
        check(game.playerPosition().y > .19f, "Step fixture never stepped up");
        move.jump = true; game.update(move, 1.f/120); move.jump = false;
        float peak = game.playerPosition().y;
        for (int t = 0; t < 80; ++t) { game.update(move, 1.f/120); peak = std::max(peak, game.playerPosition().y); }
        check(peak > .9f, "Live box step grace swallowed a jump");
        if (argc >= 3) {
            host::SceneDocument document; std::string error;
            check(host::loadScene(argv[2], document, error), error.c_str());
            game.initialize(document.scene); game.setCharacter(document.scene.characterAssets.front(), true); settle(game);
            unsigned supported = 0, missingGround = 0, contacts = 0, jumps = 0;
            float slip = 0, penetration = 0, rise = 0;
            Input input; input.forward = 1;
            for (unsigned t = 0; t < 720; ++t) {
                input.forward = t < 240 ? 1.f : (t < 480 ? -1.f : 0.f);
                input.jump = t == 60 || t == 300 || t == 540;
                const float before = game.playerPosition().y;
                const bool wasGrounded = game.grounded();
                game.update(input, 1.f/120);
                if (input.jump && wasGrounded && !game.grounded()) { ++jumps; rise = std::max(rise, game.playerPosition().y - before); }
                const auto& c = game.animationController();
                if (game.grounded()) {
                    ++supported;
                    missingGround += c.featureState().fallback == animation::RigFallback::NoGround;
                }
                for (const auto& foot : c.featureState().feet) contacts += foot.locked;
                slip = std::max(slip, c.featureMetrics().contactSlip);
                penetration = std::max(penetration, c.featureMetrics().penetration);
            }
            check(jumps == 3, "Actual project map rejected a scheduled grounded jump");
            check(missingGround == 0, "Actual map CDM could not find terrain while the body was grounded");
            check(contacts > 0, "Actual map never established a CDM foot lock");
            if (argc == 4) {
                std::ofstream report(argv[3]);
                report << "{\"success\":true,\"ticks\":720,\"jumps\":" << jumps
                       << ",\"groundedTicks\":" << supported << ",\"groundedNoGroundTicks\":" << missingGround
                       << ",\"lockedFootTicks\":" << contacts << ",\"peakContactSlipMps\":" << slip
                       << ",\"peakFootPenetrationM\":" << penetration << ",\"firstTickJumpRiseM\":" << rise << "}\n";
                check(bool(report), "Could not save actual map evidence");
            }
        }
        std::cout << "Heightfield character: " << checks << " checks passed.\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
