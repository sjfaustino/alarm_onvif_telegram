#pragma once

// Pure alert-once/re-arm decision logic behind checkSubscriptionHealth
// (telegram.cpp) - split out so it's natively unit-tested
// (test/test_subscription_health) without FreeRTOS/Arduino, same reasoning
// as background_job_state.h/backoff.h. checkSubscriptionHealth exists to
// catch a camera that keeps *answering* (refreshing CameraState::
// lastContactMs, even with a SOAP fault - see camera.cpp's cameraSoapCall)
// but can never actually hold a subscription, and so can never report a
// real motion/tamper/signal-loss event - a failure mode the OFFLINE alert
// (driven by lastContactMs alone) can't catch on its own.
//
// msSinceLastSubscribed is the caller's own millis() - CameraState::
// lastSubscribedMs, not computed here - keeps this function free of any
// time-source dependency, same convention detectorSafeBackoffCapMs
// (backoff.h) already uses.
struct SubscriptionHealthResult {
  bool alerted;     // new value for CameraState::subscriptionLostAlerted
  bool shouldAlert; // true only on the one call that should actually send the Telegram alert
};

// isOffline: true means checkCameraOnlineStatus has already alerted for a
// genuinely silent camera - this always no-ops (and re-arms) in that case,
// so a camera that's both OFFLINE and never-subscribed only gets the one
// alert, not a confusing second one for the same underlying "not
// monitoring" condition.
SubscriptionHealthResult evaluateSubscriptionHealth(bool isOffline, unsigned long msSinceLastSubscribed,
                                                      unsigned long thresholdMs, bool alreadyAlerted);
