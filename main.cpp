#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <algorithm>

using namespace geode::prelude;

// Early Click Buffer v1.2 (GD 2.2081, Geode v5)
// -----------------------
// Every click is passed to the game immediately, exactly like vanilla, so
// ship / wave / swing movement is never altered.
//
// If a press arrives while the player cannot act on it yet (in the air, not
// touching an orb), it is remembered for a short window. If the player
// becomes able to act inside that window (lands, or touches an orb) and the
// button has already been released, the press is re-sent at that exact
// step, and the release is re-sent after the same hold length the player
// originally used. If the button is still held, the game's own hold-buffer
// handles it and we stay out of the way.
//
// All timing is accumulated game time (the dt of every physics step), so it
// is independent of FPS/TPS and stays correct when Click Between Frames
// splits a frame into several steps.

namespace {
	constexpr int JUMP = static_cast<int>(PlayerButton::Jump);

	struct Slot {
		bool  pending    = false; // press not yet able to act
		bool  physDown   = false; // is the player physically holding the button
		float windowLeft = 0.f;   // time left to use the buffered press
		float heldFor    = 0.f;   // how long the button was held (pending phase)
		float releaseIn  = -1.f;  // >= 0: scheduled release of an injected press
	};

	// ---- the only places that read player state ----
	// If your Geode bindings name these differently, fix them here.
	bool onRing(PlayerObject* p) {
		return p->m_touchingRings && p->m_touchingRings->count() > 0;
	}

	bool onSurface(PlayerObject* p) {
		// Modes where a click only does something on a surface:
		// cube, ball, robot, spider. (Ship, UFO, wave and swing can act
		// anywhere, so only orbs matter for them.)
		bool surfaceMode = !(p->m_isShip || p->m_isBird || p->m_isDart || p->m_isSwing);
		return surfaceMode && p->m_isOnGround;
	}
}

class $modify(BufferedLayer, GJBaseGameLayer) {
	struct Fields {
		Slot slots[2];
		bool injecting = false;
	};

	static void onModify(auto& self) {
		// Run closest to the original function so we see the final calls,
		// including the sub-step calls made by Click Between Frames.
		(void)self.setHookPriority("GJBaseGameLayer::handleButton", Priority::Last);
		(void)self.setHookPriority("GJBaseGameLayer::processCommands", Priority::Last);
	}

	bool bufferActive() {
		return Mod::get()->getSettingValue<bool>("enabled") && PlayLayer::get() != nullptr;
	}

	bool playerCanAct(PlayerObject* p) {
		if (!p) return false;
		auto mod = Mod::get();
		if (mod->getSettingValue<bool>("orbs") && onRing(p)) return true;
		if (mod->getSettingValue<bool>("ground") && onSurface(p)) return true;
		return false;
	}

	bool canAct(bool isPlayer1) {
		// Single-input dual mode drives both icons with one button
		bool mirrored = m_gameState.m_isDualMode && !m_levelSettings->m_twoPlayerMode;
		if (mirrored) return playerCanAct(m_player1) || playerCanAct(m_player2);
		return playerCanAct(isPlayer1 ? m_player1 : m_player2);
	}

	void inject(bool down, bool isPlayer1) {
		auto f = m_fields.self();
		f->injecting = true;
		this->handleButton(down, JUMP, isPlayer1);
		f->injecting = false;
	}

	void handleButton(bool down, int button, bool isPlayer1) {
		auto f = m_fields.self();

		if (f->injecting || button != JUMP || !bufferActive()) {
			GJBaseGameLayer::handleButton(down, button, isPlayer1);
			return;
		}

		auto& s = f->slots[isPlayer1 ? 0 : 1];

		if (down) {
			bool actsNow = canAct(isPlayer1);
			s.physDown   = true;
			s.pending    = !actsNow;
			s.heldFor    = 0.f;
			s.releaseIn  = -1.f; // a real press overrides any scheduled release
			s.windowLeft = Mod::get()->getSettingValue<int64_t>("window-ms") / 1000.f;
		} else {
			s.physDown = false;
		}

		GJBaseGameLayer::handleButton(down, button, isPlayer1);
	}

	void tickSlot(Slot& s, bool isPlayer1, float dt) {
		// 1. Release of a press we injected earlier
		if (s.releaseIn >= 0.f) {
			s.releaseIn -= dt;
			if (s.releaseIn < 0.f) {
				s.releaseIn = -1.f;
				inject(false, isPlayer1);
			}
		}

		// 2. Buffered press waiting for the player to be able to act
		if (!s.pending) return;

		if (s.physDown) s.heldFor += dt;
		s.windowLeft -= dt;
		if (s.windowLeft < 0.f) {
			s.pending = false;
			return;
		}

		if (!canAct(isPlayer1)) return;

		s.pending = false;
		if (s.physDown) return; // still held: the game's own hold-buffer applies

		inject(true, isPlayer1);
		s.releaseIn = std::max(s.heldFor, dt);
	}

	void processCommands(float dt) {
		if (bufferActive()) {
			auto f = m_fields.self();
			tickSlot(f->slots[0], true, dt);
			tickSlot(f->slots[1], false, dt);
		}
		GJBaseGameLayer::processCommands(dt);
	}
};
