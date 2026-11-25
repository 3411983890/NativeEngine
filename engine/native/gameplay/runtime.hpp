#pragma once
#include "world.hpp"
#include <filesystem>

namespace engine::gameplay {
struct Module {
    Json descriptor;
    std::string source;
    std::string hash;
};
struct Behavior {
    std::string entity, id, script;
    Json properties = Json::object(), state = Json::object(), rng = Json::object();
    bool started = false, disabled = false;
};
struct Limits {
    unsigned instructions = 100000;
    std::size_t memoryBytes = 4 * 1024 * 1024;
    unsigned commands = 128, events = 4096, timers = 4096;
};
struct NativeSystem {
    std::string name;
    Phase phase=Phase::Simulation;
    std::vector<std::string> after;
    std::function<void(class Runtime&)> run;
};
class Runtime {
public:
    World world;
    Limits limits;
    void initialize(const std::filesystem::path& assetRoot, std::uint64_t seed = 1);
    void loadModule(const std::string& name, Json descriptor, std::string source);
    void attach(const std::string& entity, const std::string& id, const std::string& script, const Json& properties = Json::object());
    void start();
    void tick(const Json& input = Json::object());
    void interact(const std::string& target, const std::string& actor);
    void collectTriggers(const std::string& actor);
    void emit(const std::string& target, const std::string& name, const Json& payload = Json::object());
    void destroy(const std::string& entity);
    // Only call between ticks; failure leaves modules and all instances untouched.
    bool reload(const std::map<std::string, Module>& replacements);
    bool pollReload();
    void lockContent(bool locked) { locked_ = locked; }
    bool contentLocked() const { return locked_; }
    void registerSystem(NativeSystem system);
    Json snapshot() const;
    void restore(const Json& state);
    void save(const std::string& path) const;
    void load(const std::string& path);
    Json manifest() const;
    std::string digest() const;
    Json diagnostics() const { return diagnostics_; }
    Json takePresentation();
    const std::vector<Behavior>& behaviors() const { return behaviors_; }
    std::uint64_t ticks() const { return tick_; }
    Json inspect() const;
    void reportDiagnostic(const std::string& code,const std::string& message) { note(code,message); }
private:
    struct Invocation { Json state, commands, rng; };
    Invocation execute(const Module& module, const Behavior& b, const std::string& callback, const Json& event);
    void invoke(std::size_t index, const std::string& callback, const Json& event = Json::object());
    void commit(const Json& commands);
    void flushStructural();
    Module readModule(const std::string& name) const;
    void resolveModule(const std::string& name, std::set<std::string>& resolving, std::set<std::string>& resolved);
    void note(const std::string& code, const std::string& message, const Behavior* b = nullptr);
    std::filesystem::path root_;
    std::map<std::string, Module> modules_;
    std::vector<Behavior> behaviors_;
    std::vector<NativeSystem> systems_;
    Json structural_=Json::array();
    Json events_ = Json::array(), timers_ = Json::array(), presentation_ = Json::array(), diagnostics_ = Json::array();
    Json overlaps_ = Json::object(), input_ = Json::object();
    std::uint64_t seed_ = 1, tick_ = 0, sequence_ = 0;
    bool locked_ = false, running_ = false, tearingDown_=false;
};
}
