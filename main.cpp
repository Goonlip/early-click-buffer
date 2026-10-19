#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/ui/Notification.hpp>
#include <algorithm>
#include <cmath>
#include <vector>

using namespace geode::prelude;

// Auto Click Timing v2
// --------------------
// Learns, by trial and error, which click timings get you past the spots
// where you keep dying, then delays your early clicks so they land on the
// learned time.
//
// 1. You die. The mod blames the last click (press or release) before the
//    death and remembers it as a "problem" with the time of death.
// 2. On the next attempts, when you click near that spot, the mod delays
//    the click by a little more each time (4 ms, 8 ms, 12 ms, ...).
// 3. If you get past the old death spot, that landing time is saved. From
//    then on, any click you make up to "max shift" ms BEFORE the saved time
//    is held back and applied exactly at the saved time.
// 4. If the whole range fails, the mod gives up on that click and blames the
//    one before it.
//
// A click can only be delayed, never moved earlier, so this fixes clicks
// that were too EARLY. Everything is passed through untouched otherwise.
// Learned data is kept while you stay in the level (including practice
// mode) and is cleared when you leave it.

namespace {
	constexpr int JUMP = static_cast<int>(PlayerButton::Jump);
	constexpr double EPS = 1e-6;
	constexpr int MAX_BLAME_DEPTH = 3;     // how many clicks back we may blame
	constexpr double MAX_BLAME_AGE = 0.6;  // ignore clicks older than this before death

	bool enabled() { return Mod::get()->getSettingValue<bool>("enabled"); }
	bool debugOn() { return Mod::get()->getSettingValue<bool>("debug"); }
	double maxShift() { return Mod::get()->getSettingValue<int64_t>("max-shift-ms") / 1000.0; }
	double stepSize() { return Mod::get()->getSettingValue<int64_t>("search-step-ms") / 1000.0; }

	void say(std::string const& msg) {
		log::info("[ACT] {}", msg);
		if (debugOn()) {
			Notification::create("ACT: " + msg, NotificationIcon::Info, 1.5f)->show();
		}
	}

	// Level time in seconds. If the game state has no m_levelTime field in
	// your bindings, this falls back to a clock we keep ourselves (which is
	// less accurate with checkpoints).
	template <class GS>
	double readLevelTime(GS& gs, double fallback) {
		if constexpr (requires { gs.m_levelTime; }) {
			return static_cast<double>(gs.m_levelTime);
		} else {
			return fallback;
		}
	}

	struct Problem {
		int pl = 0;
		bool down = true;
		double anchor = 0;     // raw time of the blamed click
		double deathTime = 0;  // when we died after it
		double stepSec = 0.004;
		int k = 1;             // current trial: target = anchor + k * step
		int kMax = 8;
		bool solved = false;
		bool hopeless = false;
		bool applied = false;  // a delay for this problem was used this attempt
		double appliedAt = 0;
		double target() const { return anchor + k * stepSec; }
	};

	struct Event  { double raw; double eff; bool down; int pl; };
	struct Queued { double applyAt; double raw; bool down; int pl; };

	struct Learner {
		std::vector<Problem> problems;
		std::vector<Event> events;   // clicks that reached the game this attempt
		std::vector<Queued> queue;   // clicks being held back
		double lastShift[2] = {0, 0};
		double lastApply[2] = {-1, -1};
		bool deathHandled = false;
		void* bound = nullptr;
		double fallbackClock = 0;

		void bind(void* level) {
			if (bound != level) {
				*this = Learner();
				bound = level;
			}
		}

		void wipe() { *this = Learner(); }

		void onReset() {
			events.clear();
			queue.clear();
			lastShift[0] = lastShift[1] = 0;
			lastApply[0] = lastApply[1] = -1;
			deathHandled = false;
			fallbackClock = 0;
			for (auto& p : problems) p.applied = false;
		}

		bool hasQueued(int pl) const {
			for (auto& q : queue) if (q.pl == pl) return true;
			return false;
		}

		int pickProblem(int pl, bool down, double t) const {
			int best = -1;
			double bestDist = 1e9;
			double win = maxShift();
			for (size_t i = 0; i < problems.size(); i++) {
				auto& p = problems[i];
				if (p.hopeless || p.pl != pl || p.down != down) continue;
				double E = p.target();
				if (t > E + EPS) continue;           // too late to help
				if (E - t > win + EPS) continue;     // too early for the window
				double d = std::abs(t - p.anchor);
				if (d < bestDist) { bestDist = d; best = static_cast<int>(i); }
			}
			return best;
		}

		// Called for every jump-button event. Returns true if the event should
		// be applied to the game right now, false if it was queued.
		bool route(bool down, int pl, double t) {
			int pi = pickProblem(pl, down, t);
			double delay = 0;
			if (pi >= 0) {
				delay = problems[pi].target() - t;
			} else if (!down) {
				delay = lastShift[pl]; // keep the hold length of a delayed press
			}
			if (down) lastShift[pl] = (pi >= 0) ? delay : 0;

			if (pi >= 0) {
				auto& p = problems[pi];
				if (!p.applied) {
					say(fmt::format("trying click +{:.0f} ms", p.k * p.stepSec * 1000.0));
				}
				p.applied = true;
				p.appliedAt = t + delay;
			}

			double applyAt = t + delay;
			bool queued = hasQueued(pl);
			if (queued) applyAt = std::max(applyAt, lastApply[pl]);

			if (!queued && delay <= EPS) {
				events.push_back({t, t, down, pl});
				return true;
			}
			queue.push_back({applyAt, t, down, pl});
			lastApply[pl] = applyAt;
			return false;
		}

		template <class F>
		void flush(double now, double half, F&& send) {
			for (size_t i = 0; i < queue.size();) {
				if (queue[i].applyAt <= now + half) {
					Queued q = queue[i];
					queue.erase(queue.begin() + i);
					events.push_back({q.raw, now, q.down, q.pl});
					send(q.down, q.pl == 0);
				} else {
					i++;
				}
			}
		}

		void onDeath(double D) {
			if (deathHandled) return;
			deathHandled = true;
			const double margin = std::max(0.15, maxShift() + 0.05);

			// 1. Judge the trials that were running this attempt
			for (auto& p : problems) {
				if (!p.applied || p.hopeless) continue;
				if (D >= p.deathTime + margin) {
					if (!p.solved) {
						say(fmt::format("learned a fix: click +{:.0f} ms", p.k * p.stepSec * 1000.0));
					}
					p.solved = true;
				} else if (D >= p.appliedAt) {
					p.solved = false;
					p.k++;
					if (p.k > p.kMax) {
						p.hopeless = true;
						say("that click can't be fixed by delaying it");
					}
				} // else: died before the delayed click took effect, no information
			}

			// 2. Is this death already being handled by an existing problem?
			for (auto& p : problems) {
				if (!p.hopeless && std::abs(D - p.deathTime) < margin) return;
			}

			// 3. New death spot: blame the most recent click we haven't given up on
			int considered = 0;
			for (int i = static_cast<int>(events.size()) - 1;
			     i >= 0 && considered < MAX_BLAME_DEPTH; i--) {
				auto& e = events[i];
				if (D - e.eff > MAX_BLAME_AGE) break;
				considered++;

				bool givenUp = false;
				for (auto& p : problems) {
					if (p.hopeless && p.pl == e.pl && p.down == e.down &&
					    std::abs(p.anchor - e.raw) < maxShift() + EPS) givenUp = true;
				}
				if (givenUp) continue;

				Problem np;
				np.pl = e.pl;
				np.down = e.down;
				np.anchor = e.raw;
				np.deathTime = D;
				np.stepSec = std::max(0.001, stepSize());
				np.kMax = std::max(1, static_cast<int>(std::floor(maxShift() / np.stepSec)));
				problems.push_back(np);
				say(fmt::format("new problem at {:.2f}s, will test delays", D));
				return;
			}
		}

		void onComplete() {
			for (auto& p : problems) {
				if (p.applied && !p.hopeless) {
					if (!p.solved) {
						say(fmt::format("learned a fix: click +{:.0f} ms", p.k * p.stepSec * 1000.0));
					}
					p.solved = true;
				}
			}
		}
	};

	Learner& L() {
		static Learner l;
		return l;
	}
}

class $modify(TimingLayer, GJBaseGameLayer) {
	struct Fields {
		bool injecting = false;
	};

	static void onModify(auto& self) {
		// Run closest to the original so we see the final calls (this also
		// keeps us compatible with Click Between Frames).
		(void)self.setHookPriority("GJBaseGameLayer::handleButton", Priority::Last);
		(void)self.setHookPriority("GJBaseGameLayer::processCommands", Priority::Last);
	}

	bool active() {
		return enabled() && PlayLayer::get() != nullptr;
	}

	double nowTime() {
		return readLevelTime(m_gameState, L().fallbackClock);
	}

	void inject(bool down, bool isPlayer1) {
		auto f = m_fields.self();
		f->injecting = true;
		this->handleButton(down, JUMP, isPlayer1);
		f->injecting = false;
	}

	void handleButton(bool down, int button, bool isPlayer1) {
		auto f = m_fields.self();
		if (f->injecting || button != JUMP || !active()) {
			GJBaseGameLayer::handleButton(down, button, isPlayer1);
			return;
		}
		int pl = isPlayer1 ? 0 : 1;
		if (L().route(down, pl, nowTime())) {
			GJBaseGameLayer::handleButton(down, button, isPlayer1);
		}
		// otherwise the click is held back and applied later in processCommands
	}

	void processCommands(float dt) {
		if (active()) {
			auto& l = L();
			l.bind(PlayLayer::get());
			l.fallbackClock += dt;
			l.flush(nowTime(), dt * 0.5, [this](bool down, bool p1) {
				this->inject(down, p1);
			});
		}
		GJBaseGameLayer::processCommands(dt);
	}
};

class $modify(TimingPlayLayer, PlayLayer) {
	void resetLevel() {
		PlayLayer::resetLevel();
		if (enabled()) L().onReset();
	}

	void destroyPlayer(PlayerObject* player, GameObject* obj) {
		PlayLayer::destroyPlayer(player, obj);
		if (enabled()) {
			L().onDeath(readLevelTime(m_gameState, L().fallbackClock));
		}
	}

	void levelComplete() {
		if (enabled()) L().onComplete();
		PlayLayer::levelComplete();
	}

	void onQuit() {
		PlayLayer::onQuit();
		L().wipe();
	}
};
