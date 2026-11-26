#include "binder.hpp"

namespace g6rt::bind {

namespace {
struct Exclusive {
    SRWLOCK* l;
    explicit Exclusive(SRWLOCK* x) noexcept : l(x) { AcquireSRWLockExclusive(l); }
    ~Exclusive() { ReleaseSRWLockExclusive(l); }
    Exclusive(const Exclusive&) = delete;
    Exclusive& operator=(const Exclusive&) = delete;
};
struct Shared {
    SRWLOCK* l;
    explicit Shared(SRWLOCK* x) noexcept : l(x) { AcquireSRWLockShared(l); }
    ~Shared() { ReleaseSRWLockShared(l); }
    Shared(const Shared&) = delete;
    Shared& operator=(const Shared&) = delete;
};
// Callers take `now` before the lock, so another thread may already have stored a later time.
inline std::uint64_t since(std::uint64_t now, std::uint64_t t) noexcept { return now > t ? now - t : 0; }
} // namespace

void Binder::configure(const Config& c) noexcept {
    Exclusive g(&lock_);
    cfg_ = c;
    if (cfg_.gap_ms == 0) cfg_.gap_ms = 1;
    n_ = 0;
    bound_ = 0;
    alive_seq_ = any_seq_ = 0;
    last_sweep_ = 0;
    counts_ = { 0, 0, 0 };
}

std::uint32_t Binder::bound_id() const noexcept { Shared g(&lock_); return bound_; }
std::uint32_t Binder::generation() const noexcept { Shared g(&lock_); return gen_; }
std::uint32_t Binder::tracked() const noexcept { Shared g(&lock_); return n_; }
Binder::Counts Binder::counts() const noexcept { Shared g(&lock_); return counts_; }

Binder::Entry* Binder::find(std::uint32_t id) noexcept {
    for (std::uint32_t i = 0; i < n_; ++i)
        if (t_[i].id == id) return &t_[i];
    return nullptr;
}

Binder::Entry* Binder::insert(std::uint32_t id, std::uint32_t name, std::uint64_t now) noexcept {
    if (n_ == kMaxThreads) {
        // Full: drop the least recently seen thread that is not bound.
        std::uint32_t victim = kMaxThreads;
        for (std::uint32_t i = 0; i < n_; ++i)
            if (t_[i].id != bound_ && (victim == kMaxThreads || t_[i].last_ms < t_[victim].last_ms)) victim = i;
        if (victim == kMaxThreads) return nullptr;
        t_[victim] = t_[--n_];
    }
    Entry& e = t_[n_++];
    e = Entry{};
    e.id = id;
    e.name = name;
    e.first_ms = e.last_ms = now;
    return &e;
}

void Binder::push(Decision& d, const Event& e) noexcept {
    if (d.nev < sizeof d.ev / sizeof d.ev[0]) d.ev[d.nev++] = e;
}

void Binder::unbind(Decision& d, const Entry* e, const char* why) noexcept {
    Event ev;
    ev.kind = Event::unbound;
    ev.id = bound_;
    ev.generation = gen_;
    ev.why = why;
    if (e) { ev.name = e->name; ev.state = e->state; ev.runs = e->runs; }
    push(d, ev);
    bound_ = 0;
    ++counts_.unbinds;
}

void Binder::choose(Decision& d, std::uint64_t now) noexcept {
    if (cfg_.mode == Mode::observe || bound_ != 0) return;
    const Entry* best = nullptr;
    std::uint64_t best_len = 0;
    for (std::uint32_t i = 0; i < n_; ++i) {
        const Entry& e = t_[i];
        if (!e.alive_seen || !state_alive(e.state)) continue;
        if (cfg_.mode == Mode::hash && e.name != cfg_.hash) continue;
        if (since(now, e.alive_ms) >= cfg_.gap_ms) continue;               // not present right now
        const std::uint64_t len = e.alive_ms - e.streak_ms;
        if (len < cfg_.observe_ms) continue;
        if (!best || len > best_len || (len == best_len && (e.alive_runs > best->alive_runs ||
                                                            (e.alive_runs == best->alive_runs && e.id < best->id)))) {
            best = &e;
            best_len = len;
        }
    }
    if (!best) return;
    bound_ = best->id;
    ++gen_;
    ++counts_.binds;
    Event ev;
    ev.kind = Event::bound;
    ev.id = best->id;
    ev.name = best->name;
    ev.state = best->state;
    ev.generation = gen_;
    ev.presence_ms = best_len;
    ev.runs = best->alive_runs;
    ev.why = cfg_.mode == Mode::hash ? "longest continuous presence among the host_script_hash threads"
                                     : "longest continuous presence";
    push(d, ev);
}

void Binder::sweep(Decision& d, std::uint64_t now) noexcept {
    if (last_sweep_ != 0 && since(now, last_sweep_) < 500) return;
    last_sweep_ = now;
    for (std::uint32_t i = 0; i < n_;) {
        const Entry& e = t_[i];
        if (since(now, e.last_ms) >= cfg_.forget_ms && any_seq_ - e.any_seq >= kForgetRuns) {
            if (e.id == bound_) unbind(d, &e, "gone");
            Event ev;
            ev.kind = Event::gone;
            ev.id = e.id;
            ev.name = e.name;
            ev.state = e.state;
            ev.presence_ms = e.last_ms - e.first_ms;
            ev.runs = e.runs;
            push(d, ev);
            ++counts_.gone;
            t_[i] = t_[--n_];
            continue;
        }
        ++i;
    }
}

Decision Binder::on_run(std::uint32_t id, std::uint32_t name, std::uint8_t state, std::uint64_t now) noexcept {
    Decision d;
    Exclusive g(&lock_);
    ++any_seq_;
    const bool alive = state_alive(state);
    if (alive) ++alive_seq_;
    Entry* e = find(id);
    if (!e) e = insert(id, name, now);
    if (e) {
        if (now > e->last_ms) e->last_ms = now;
        e->name = name;
        e->state = state;
        e->any_seq = any_seq_;
        ++e->runs;
        if (alive) {
            if (!e->alive_seen || since(now, e->alive_ms) >= cfg_.gap_ms) e->streak_ms = now;
            e->alive_seen = true;
            if (now > e->alive_ms) e->alive_ms = now;
            e->alive_seq = alive_seq_;
            ++e->alive_runs;
        }
    }
    if (bound_ != 0) {
        const Entry* b = find(bound_);
        if (!b) unbind(d, nullptr, "no longer tracked");
        else if (state_finishing(b->state)) unbind(d, b, "finishing state");
        else if (since(now, b->alive_ms) >= cfg_.absent_ms && alive_seq_ - b->alive_seq >= kAbsentRuns)
            unbind(d, b, "absent while other threads run");
    }
    sweep(d, now);
    choose(d, now);
    d.generation = gen_;
    d.tick = cfg_.mode != Mode::observe && bound_ != 0 && bound_ == id && alive;
    return d;
}

} // namespace g6rt::bind
