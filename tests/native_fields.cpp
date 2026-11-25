#include "../engine/native/fields/world.hpp"
#include "../engine/native/simulation/clock.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace engine::fields;
using engine::simulation::Clock;

namespace {
unsigned checks = 0, groups = 0;
void check(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
template<class F> void throws(F action, const char* message) {
    bool threw = false;
    try { action(); } catch (const std::exception&) { threw = true; }
    check(threw, message);
}
Channel density(Domain domain = Domain::CpuAuthority) {
    return {"density", "fixed-opacity", "smoke", domain, Merge::Sum, 0, 1000, 1000};
}
LeaseRequest request(Region region = {{-16,0,0},{16,1,1}}, bool writer = true,
                     Consistency level = Consistency::Eventual) {
    return {region, {0}, writer ? "smoke" : "ai", writer, level, 3, 16};
}
Lease acquire(World& world, LeaseRequest req = request()) {
    auto result = world.acquire(std::move(req));
    check(result.accepted, "lease rejected");
    return result.lease;
}
void clocks() {
    for (unsigned fps : {30,60,144,240}) {
        Clock clock;
        std::uint64_t previous = 0;
        for (unsigned frame = 1; frame <= fps; ++frame) {
            const auto end = 1000000000ull * frame / fps;
            clock.addNanoseconds(end - previous);
            previous = end;
            while (clock.consumeTick()) {}
        }
        check(clock.tick() == 120 && clock.pendingTicks() == 0, "render fps changed simulation time");
        check(clock.fraction() == 0, "rational clock accumulated rounding drift");
    }
    Clock clock;
    clock.addNanoseconds(1000000000ull);
    for (int i = 0; i < 8; ++i) check(clock.consumeTick(), "work budget consume failed");
    check(clock.pendingTicks() == 112, "catch-up discarded authority debt");
    clock.setPaused(true);
    const auto state = clock.snapshot();
    clock.addNanoseconds(1000000000ull);
    check(clock.pendingTicks() == 112 && !clock.consumeTick(), "pause advanced the clock");
    Clock resumed;
    resumed.restore(state);
    resumed.setPaused(false);
    clock.setPaused(false);
    while (clock.consumeTick()) {}
    while (resumed.consumeTick()) {}
    check(resumed.tick() == clock.tick(), "snapshot lost time debt");
    throws([] { Clock invalid(0); }, "zero hz accepted");
    const auto before = clock.snapshot();
    throws([&] { clock.addNanoseconds(std::numeric_limits<std::uint64_t>::max()); }, "time overflow accepted");
    check(clock.snapshot().phase == before.phase, "failed time advance mutated clock");
    ++groups;
}
void regionalAllocation() {
    World world(4);
    world.registerChannel(density());
    world.registerChannel({"unused", "K", "fire", Domain::VisualReference, Merge::Maximum, 0, 5000, 1});
    check(world.stats().committedBytes == 0, "unleased channel allocated payload");
    auto a = acquire(world);
    check(world.stats().activePages == 2, "negative cell did not map to negative brick");
    auto b = acquire(world, request({{-1,0,0},{1,1,1}}, false));
    check(world.stats().activePages == 2, "overlapping leases duplicated physical pages");
    const auto before = world.stats();
    auto bad = request({{std::numeric_limits<std::int32_t>::min(),0,0},
                        {std::numeric_limits<std::int32_t>::max(),1,1}});
    check(!world.acquire(bad).accepted, "unbounded region accepted");
    check(world.stats().activePages == before.activePages && world.stats().leases == before.leases,
          "rejected acquisition changed allocations");
    check(world.release(a, 20), "release failed");
    check(world.release(b, 3), "last release failed");
    check(world.stats().activePages == 0 && world.stats().retiredPages == 2, "pages not retired");
    world.collect(19);
    check(world.stats().retiredPages == 2, "shared page freed before highest reader fence");
    world.collect(20);
    check(world.stats().committedBytes == 0, "released payload leaked");
    auto c = acquire(world);
    check(!(c == a), "lease generation reused");
    check(!world.release(a), "old lease released replacement");
    check(world.sample(a,0,{0,0,0},0).status == Status::InvalidLease, "stale handle could read");
    check(world.release(c), "replacement release failed");
    world.collect(20);
    for (int i = 0; i < 10000; ++i) {
        auto lease = acquire(world, request({{0,0,0},{1,1,1}}));
        check(world.release(lease), "lease cycle failed");
        world.collect(20);
    }
    check(world.stats().committedBytes == 0 && world.stats().leases == 0, "10k lease cycles leaked");
    ++groups;
}
void publicationBarrier() {
    World world;
    world.registerChannel(density());
    auto writer = acquire(world);
    auto reader = acquire(world, request({{-16,0,0},{16,1,1}}, false, Consistency::Strong));
    check(world.publish(writer,0,1,1,10,Domain::CpuAuthority,{{{-1,0,0},125},{{0,0,0},250}}) == PublishResult::Accepted,
          "initial publish failed");
    check(!world.sample(reader,0,{0,0,0},10).valid(), "staging leaked before commit");
    check(!world.commit(9), "future source committed too early");
    check(world.commit(10), "commit failed");
    check(world.sample(reader,0,{-1,0,0},10).value == 125, "negative cell sampled incorrectly");
    check(world.sample(reader,0,{0,0,0},10).value == 250, "published cell missing");
    check(world.sample(reader,0,{1,0,0},10).status == Status::Unavailable, "unknown hazard became zero");
    check(world.sample(reader,0,{16,0,0},10).status == Status::OutsideRegion, "exclusive region boundary read");
    check(world.sample(reader,0,{0,0,0},11).status == Status::TickMismatch, "Strong served wrong snapshot");
    check(world.publish(writer,0,1,2,11,Domain::CpuAuthority,{{{0,0,0},500}}) == PublishResult::Accepted,
          "replacement rejected");
    check(world.sample(reader,0,{0,0,0},10).value == 250, "replacement modified old snapshot");
    check(world.commit(11), "second commit failed");
    check(world.sample(reader,0,{0,0,0},11).value == 500, "replacement accumulated instead of replacing");
    check(!world.sample(reader,0,{-1,0,0},11).valid(), "old source footprint survived replacement");
    check(!world.commit(11), "duplicate tick commit accepted");
    ++groups;
}
void freshnessAndPermissions() {
    World world;
    world.registerChannel(density());
    auto writer = acquire(world);
    auto reader = acquire(world, request({{-16,0,0},{16,1,1}}, false));
    check(world.publish(writer,0,1,1,1,Domain::VisualReference,{{{0,0,0},10}}) == PublishResult::AccessDenied,
          "visual publication polluted authority");
    check(world.publish(reader,0,1,1,1,Domain::CpuAuthority,{{{0,0,0},10}}) == PublishResult::AccessDenied,
          "read-only lease wrote");
    auto wrong = request(); wrong.owner = "other";
    check(!world.acquire(wrong).accepted, "non-owner acquired write permission");
    check(world.publish(writer,0,1,1,1,Domain::CpuAuthority,{{{0,0,0},10}}) == PublishResult::Accepted,
          "authority publication failed");
    check(world.commit(1), "commit failed");
    check(world.sample(reader,0,{0,0,0},4).valid(), "within-age Eventual unavailable");
    const auto stale = world.sample(reader,0,{0,0,0},5);
    check(stale.status == Status::Stale && stale.ageTicks == 4, "overdue sample silently valid");
    auto tolerant = acquire(world, request({{-16,0,0},{16,1,1}},false,Consistency::Stale));
    check(world.sample(tolerant,0,{0,0,0},500).valid(), "visual stale reader could not opt in");
    check(world.publish(writer,0,2,1,1,Domain::CpuAuthority,{{{0,0,0},30}}) == PublishResult::PastTick,
          "publication rewrote committed tick");
    ++groups;
}
void idempotenceAndWithdrawal() {
    World world;
    world.registerChannel(density());
    auto writer = acquire(world);
    const std::vector<Write> writes = {{{0,0,0},700},{{1,0,0},50}};
    check(world.publish(writer,0,1,1,1,Domain::CpuAuthority,writes) == PublishResult::Accepted, "publish failed");
    auto reversed = writes; std::reverse(reversed.begin(), reversed.end());
    check(world.publish(writer,0,1,1,1,Domain::CpuAuthority,reversed) == PublishResult::Duplicate,
          "reordered retransmission counted twice");
    check(world.publish(writer,0,1,1,1,Domain::CpuAuthority,{{{0,0,0},500}}) == PublishResult::Conflict,
          "same epoch conflicting payload accepted");
    check(world.publish(writer,0,2,1,1,Domain::CpuAuthority,{{{0,0,0},700}}) == PublishResult::Accepted, "second source failed");
    check(world.commit(1), "commit failed");
    auto combined = world.sample(writer,0,{0,0,0},1);
    check(combined.value == 1000 && combined.clamped, "sum clamp not explicit");
    check(world.publish(writer,0,1,2,2,Domain::CpuAuthority,{}) == PublishResult::Accepted, "withdrawal rejected");
    world.collect(0); // must not forget withdrawal tombstone while owner remains active
    check(world.publish(writer,0,1,1,1,Domain::CpuAuthority,writes) == PublishResult::OldEpoch, "withdrawn source resurrected");
    check(world.commit(2), "withdrawal commit failed");
    check(world.sample(writer,0,{0,0,0},2).value == 700, "withdrawal subtracted from saturated sum");
    auto narrow = acquire(world, request({{0,0,0},{1,1,1}}));
    check(world.publish(writer,0,3,1,3,Domain::CpuAuthority,{{{0,0,0},50},{{1,0,0},50}}) == PublishResult::Accepted, "regional source failed");
    check(world.publish(narrow,0,3,2,3,Domain::CpuAuthority,{{{0,0,0},60}}) == PublishResult::AccessDenied,
          "narrow writer withdrew cells outside region in same brick");
    const auto previous = world.stats().publications;
    check(world.publish(writer,0,4,1,3,Domain::CpuAuthority,{{{0,0,0},1},{{0,0,0},2}}) == PublishResult::Invalid,
          "duplicate cell accepted");
    check(world.stats().publications == previous, "invalid transaction partially published");
    ++groups;
}
void budgetAndInFlight() {
    World world(1,1);
    world.registerChannel(density());
    auto writer = acquire(world, request({{0,0,0},{16,1,1}}));
    check(world.publish(writer,0,1,1,1,Domain::CpuAuthority,{{{0,0,0},10}}) == PublishResult::Accepted, "first tile failed");
    check(world.publish(writer,0,2,1,1,Domain::CpuAuthority,{{{0,0,0},20}}) == PublishResult::CapacityExceeded, "tile budget ignored");
    check(world.publish(writer,0,1,2,1,Domain::CpuAuthority,{{{0,0,0},30}}) == PublishResult::Accepted, "replacement incorrectly needs extra tile");
    check(world.release(writer,10), "release failed");
    check(!world.acquire(request({{0,0,0},{16,1,1}})).accepted, "in-flight physical page reused");
    world.collect(9);
    check(world.stats().retiredPages == 1, "page freed early");
    world.collect(10);
    check(world.stats().committedBytes == 0, "retired source memory leaked");
    auto next = acquire(world, request({{0,0,0},{16,1,1}}));
    check(world.publish(next,0,1,1,2,Domain::CpuAuthority,{{{0,0,0},5}}) == PublishResult::Accepted, "new owner lifetime cannot republish");
    ++groups;
}
void deterministicContributions() {
    std::uint64_t reference = 0;
    for (unsigned run = 0; run < 100; ++run) {
        World world;
        world.registerChannel(density());
        const auto visual = world.registerChannel({"visual", "density", "smoke", Domain::VisualReference, Merge::Sum, 0,1000,1});
        auto writer = acquire(world);
        auto vr = request(); vr.channels = {visual};
        auto visualWriter = acquire(world,vr);
        std::vector<unsigned> order = {1,2,3,4,5,6,7,8};
        std::rotate(order.begin(),order.begin()+(run%order.size()),order.end());
        if (run%2) std::reverse(order.begin(),order.end());
        for (auto source : order) check(world.publish(writer,0,source,1,5,Domain::CpuAuthority,
            {{{-1,0,0},static_cast<std::int32_t>(source*10)},{{0,0,0},static_cast<std::int32_t>(source)}}) == PublishResult::Accepted,
            "shuffled source publication failed");
        check(world.publish(visualWriter,visual,1,1,5,Domain::VisualReference,{{{0,0,0},static_cast<std::int32_t>(run)}}) == PublishResult::Accepted,
              "visual reference publish failed");
        check(world.commit(5), "commit failed");
        check(world.sample(writer,0,{-1,0,0},5).value == 360, "source sum incorrect");
        if (run == 0) reference = world.digest();
        check(world.digest() == reference, "publication order or visual state changed authority digest");
    }
    ++groups;
}
void signedAggregationAndMax() {
    World world;
    world.registerChannel({"signed", "fixed", "smoke",Domain::CpuAuthority,Merge::Sum,-100,100,1});
    auto lease = acquire(world);
    check(world.publish(lease,0,1,1,1,Domain::CpuAuthority,{{{0,0,0},100}}) == PublishResult::Accepted,"signed source failed");
    check(world.publish(lease,0,2,1,1,Domain::CpuAuthority,{{{0,0,0},100}}) == PublishResult::Accepted,"signed source failed");
    check(world.publish(lease,0,3,1,1,Domain::CpuAuthority,{{{0,0,0},-100}}) == PublishResult::Accepted,"negative source failed");
    check(world.commit(1),"signed commit failed");
    check(world.sample(lease,0,{0,0,0},1).value == 100,"per-source saturation made sum order dependent");
    World maximum;
    maximum.registerChannel({"max", "fixed", "smoke",Domain::CpuAuthority,Merge::Maximum,-100,100,1});
    auto m = acquire(maximum);
    maximum.publish(m,0,1,1,1,Domain::CpuAuthority,{{{0,0,0},-70}});
    maximum.publish(m,0,2,1,1,Domain::CpuAuthority,{{{0,0,0},-30}});
    check(maximum.commit(1),"maximum commit failed");
    check(maximum.sample(m,0,{0,0,0},1).value == -30,"negative maximum incorrectly initialized to zero");
    ++groups;
}
}
#include "native_field_projection.hpp"
#include "native_field_sampling.hpp"

int main() {
    try {
        clocks(); regionalAllocation(); publicationBarrier(); freshnessAndPermissions();
        idempotenceAndWithdrawal(); budgetAndInFlight(); deterministicContributions(); signedAggregationAndMax();
        projectionTests();
        samplingTests();
        std::cout << "{\"schema\":\"nativegpu.fields-reference-evidence/1\",\"ok\":true,\"groups\":" << groups
                  << ",\"checks\":" << checks << ",\"leaseCycles\":10000,\"determinismRuns\":100,"
                  << "\"projectionReference\":true,\"projectionCycles\":1000,\"persistentOwnerCycles\":1000,\"promotionPermutationRuns\":100,"
                  << "\"fixedPointTrilinear\":true,\"readHalos\":true,\"gpuIntegrated\":false,\"hostIntegrated\":false}" << std::endl;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Fields reference failed after " << checks << " checks: " << error.what() << std::endl;
        return 1;
    }
}
