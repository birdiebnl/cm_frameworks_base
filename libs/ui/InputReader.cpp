//
// Copyright 2010 The Android Open Source Project
//
// The input reader.
//
#define LOG_TAG "InputReader"

//#define LOG_NDEBUG 0

// Log debug messages for each raw event received from the EventHub.
#define DEBUG_RAW_EVENTS 0

// Log debug messages about touch screen filtering hacks.
#define DEBUG_HACKS 0

// Log debug messages about virtual key processing.
#define DEBUG_VIRTUAL_KEYS 0

// Log debug messages about pointers.
#define DEBUG_POINTERS 0

// Log debug messages about pointer assignment calculations.
#define DEBUG_POINTER_ASSIGNMENT 0

#include <cutils/log.h>
#include <ui/InputReader.h>

#include <stddef.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>

/** Amount that trackball needs to move in order to generate a key event. */
#define TRACKBALL_MOVEMENT_THRESHOLD 6


namespace android {

// --- Static Functions ---

template<typename T>
inline static T abs(const T& value) {
    return value < 0 ? - value : value;
}

template<typename T>
inline static T min(const T& a, const T& b) {
    return a < b ? a : b;
}

template<typename T>
inline static void swap(T& a, T& b) {
    T temp = a;
    a = b;
    b = temp;
}


int32_t updateMetaState(int32_t keyCode, bool down, int32_t oldMetaState) {
    int32_t mask;
    switch (keyCode) {
    case AKEYCODE_ALT_LEFT:
        mask = META_ALT_LEFT_ON;
        break;
    case AKEYCODE_ALT_RIGHT:
        mask = META_ALT_RIGHT_ON;
        break;
    case AKEYCODE_SHIFT_LEFT:
        mask = META_SHIFT_LEFT_ON;
        break;
    case AKEYCODE_SHIFT_RIGHT:
        mask = META_SHIFT_RIGHT_ON;
        break;
    case AKEYCODE_SYM:
        mask = META_SYM_ON;
        break;
    default:
        return oldMetaState;
    }

    int32_t newMetaState = down ? oldMetaState | mask : oldMetaState & ~ mask
            & ~ (META_ALT_ON | META_SHIFT_ON);

    if (newMetaState & (META_ALT_LEFT_ON | META_ALT_RIGHT_ON)) {
        newMetaState |= META_ALT_ON;
    }

    if (newMetaState & (META_SHIFT_LEFT_ON | META_SHIFT_RIGHT_ON)) {
        newMetaState |= META_SHIFT_ON;
    }

    return newMetaState;
}

static const int32_t keyCodeRotationMap[][4] = {
        // key codes enumerated counter-clockwise with the original (unrotated) key first
        // no rotation,        90 degree rotation,  180 degree rotation, 270 degree rotation
        { AKEYCODE_DPAD_DOWN,   AKEYCODE_DPAD_RIGHT,  AKEYCODE_DPAD_UP,     AKEYCODE_DPAD_LEFT },
        { AKEYCODE_DPAD_RIGHT,  AKEYCODE_DPAD_UP,     AKEYCODE_DPAD_LEFT,   AKEYCODE_DPAD_DOWN },
        { AKEYCODE_DPAD_UP,     AKEYCODE_DPAD_LEFT,   AKEYCODE_DPAD_DOWN,   AKEYCODE_DPAD_RIGHT },
        { AKEYCODE_DPAD_LEFT,   AKEYCODE_DPAD_DOWN,   AKEYCODE_DPAD_RIGHT,  AKEYCODE_DPAD_UP },
};
static const int keyCodeRotationMapSize =
        sizeof(keyCodeRotationMap) / sizeof(keyCodeRotationMap[0]);

int32_t rotateKeyCode(int32_t keyCode, int32_t orientation) {
    if (orientation != InputReaderPolicyInterface::ROTATION_0) {
        for (int i = 0; i < keyCodeRotationMapSize; i++) {
            if (keyCode == keyCodeRotationMap[i][0]) {
                return keyCodeRotationMap[i][orientation];
            }
        }
    }
    return keyCode;
}


// --- InputReader ---

InputReader::InputReader(const sp<EventHubInterface>& eventHub,
        const sp<InputReaderPolicyInterface>& policy,
        const sp<InputDispatcherInterface>& dispatcher) :
        mEventHub(eventHub), mPolicy(policy), mDispatcher(dispatcher) {
    configureExcludedDevices();
    resetGlobalMetaState();
    resetDisplayProperties();
    updateExportedVirtualKeyState();
}

InputReader::~InputReader() {
    for (size_t i = 0; i < mDevices.size(); i++) {
        delete mDevices.valueAt(i);
    }
}

void InputReader::loopOnce() {
    RawEvent rawEvent;
    mEventHub->getEvent(& rawEvent.deviceId, & rawEvent.type, & rawEvent.scanCode,
            & rawEvent.keyCode, & rawEvent.flags, & rawEvent.value, & rawEvent.when);

    // Replace the event timestamp so it is in same timebase as java.lang.System.nanoTime()
    // and android.os.SystemClock.uptimeMillis() as expected by the rest of the system.
    rawEvent.when = systemTime(SYSTEM_TIME_MONOTONIC);

#if DEBUG_RAW_EVENTS
    LOGD("Input event: device=0x%x type=0x%x scancode=%d keycode=%d value=%d",
            rawEvent.deviceId, rawEvent.type, rawEvent.scanCode, rawEvent.keyCode,
            rawEvent.value);
#endif

    process(& rawEvent);
}

void InputReader::process(const RawEvent* rawEvent) {
    switch (rawEvent->type) {
    case EventHubInterface::DEVICE_ADDED:
        handleDeviceAdded(rawEvent);
        break;

    case EventHubInterface::DEVICE_REMOVED:
        handleDeviceRemoved(rawEvent);
        break;

    case EV_SYN:
        handleSync(rawEvent);
        break;

    case EV_KEY:
        handleKey(rawEvent);
        break;

    case EV_REL:
        handleRelativeMotion(rawEvent);
        break;

    case EV_ABS:
        handleAbsoluteMotion(rawEvent);
        break;

    case EV_SW:
        handleSwitch(rawEvent);
        break;
    }
}

void InputReader::handleDeviceAdded(const RawEvent* rawEvent) {
    InputDevice* device = getDevice(rawEvent->deviceId);
    if (device) {
        LOGW("Ignoring spurious device added event for deviceId %d.", rawEvent->deviceId);
        return;
    }

    addDevice(rawEvent->when, rawEvent->deviceId);
}

void InputReader::handleDeviceRemoved(const RawEvent* rawEvent) {
    InputDevice* device = getDevice(rawEvent->deviceId);
    if (! device) {
        LOGW("Ignoring spurious device removed event for deviceId %d.", rawEvent->deviceId);
        return;
    }

    removeDevice(rawEvent->when, device);
}

void InputReader::handleSync(const RawEvent* rawEvent) {
    InputDevice* device = getNonIgnoredDevice(rawEvent->deviceId);
    if (! device) return;

    if (rawEvent->scanCode == SYN_MT_REPORT) {
        // MultiTouch Sync: The driver has returned all data for *one* of the pointers.
        // We drop pointers with pressure <= 0 since that indicates they are not down.
        if (device->isMultiTouchScreen()) {
            uint32_t pointerIndex = device->multiTouchScreen.accumulator.pointerCount;

            if (device->multiTouchScreen.accumulator.pointers[pointerIndex].fields) {
                if (pointerIndex == MAX_POINTERS) {
                    LOGW("MultiTouch device driver returned more than maximum of %d pointers.",
                            MAX_POINTERS);
                } else {
                    pointerIndex += 1;
                    device->multiTouchScreen.accumulator.pointerCount = pointerIndex;
                }
            }

            device->multiTouchScreen.accumulator.pointers[pointerIndex].clear();
        }
    } else if (rawEvent->scanCode == SYN_REPORT) {
        // General Sync: The driver has returned all data for the current event update.
        if (device->isMultiTouchScreen()) {
            if (device->multiTouchScreen.accumulator.isDirty()) {
                onMultiTouchScreenStateChanged(rawEvent->when, device);
                device->multiTouchScreen.accumulator.clear();
            }
        } else if (device->isSingleTouchScreen()) {
            if (device->singleTouchScreen.accumulator.isDirty()) {
                onSingleTouchScreenStateChanged(rawEvent->when, device);
                device->singleTouchScreen.accumulator.clear();
            }
        }

        if (device->trackball.accumulator.isDirty()) {
            onTrackballStateChanged(rawEvent->when, device);
            device->trackball.accumulator.clear();
        }
    }
}

void InputReader::handleKey(const RawEvent* rawEvent) {
    InputDevice* device = getNonIgnoredDevice(rawEvent->deviceId);
    if (! device) return;

    bool down = rawEvent->value != 0;
    int32_t scanCode = rawEvent->scanCode;

    if (device->isSingleTouchScreen()) {
        switch (rawEvent->scanCode) {
        case BTN_TOUCH:
            device->singleTouchScreen.accumulator.fields |=
                    InputDevice::SingleTouchScreenState::Accumulator::FIELD_BTN_TOUCH;
            device->singleTouchScreen.accumulator.btnTouch = down;
            return;
        }
    }

    if (device->isTrackball()) {
        switch (rawEvent->scanCode) {
        case BTN_MOUSE:
            device->trackball.accumulator.fields |=
                    InputDevice::TrackballState::Accumulator::FIELD_BTN_MOUSE;
            device->trackball.accumulator.btnMouse = down;
            return;
        }
    }

    if (device->isKeyboard()) {
        int32_t keyCode = rawEvent->keyCode;
        onKey(rawEvent->when, device, down, keyCode, scanCode, rawEvent->flags);
    }
}

void InputReader::handleRelativeMotion(const RawEvent* rawEvent) {
    InputDevice* device = getNonIgnoredDevice(rawEvent->deviceId);
    if (! device) return;

    if (device->isTrackball()) {
        switch (rawEvent->scanCode) {
        case REL_X:
            device->trackball.accumulator.fields |=
                    InputDevice::TrackballState::Accumulator::FIELD_REL_X;
            device->trackball.accumulator.relX = rawEvent->value;
            break;
        case REL_Y:
            device->trackball.accumulator.fields |=
                    InputDevice::TrackballState::Accumulator::FIELD_REL_Y;
            device->trackball.accumulator.relY = rawEvent->value;
            break;
        }
    }
}

void InputReader::handleAbsoluteMotion(const RawEvent* rawEvent) {
    InputDevice* device = getNonIgnoredDevice(rawEvent->deviceId);
    if (! device) return;

    if (device->isMultiTouchScreen()) {
        uint32_t pointerIndex = device->multiTouchScreen.accumulator.pointerCount;
        InputDevice::MultiTouchScreenState::Accumulator::Pointer* pointer =
                & device->multiTouchScreen.accumulator.pointers[pointerIndex];

        switch (rawEvent->scanCode) {
        case ABS_MT_POSITION_X:
            pointer->fields |=
                    InputDevice::MultiTouchScreenState::Accumulator::FIELD_ABS_MT_POSITION_X;
            pointer->absMTPositionX = rawEvent->value;
            break;
        case ABS_MT_POSITION_Y:
            pointer->fields |=
                    InputDevice::MultiTouchScreenState::Accumulator::FIELD_ABS_MT_POSITION_Y;
            pointer->absMTPositionY = rawEvent->value;
            break;
        case ABS_MT_TOUCH_MAJOR:
            pointer->fields |=
                    InputDevice::MultiTouchScreenState::Accumulator::FIELD_ABS_MT_TOUCH_MAJOR;
            pointer->absMTTouchMajor = rawEvent->value;
            break;
        case ABS_MT_WIDTH_MAJOR:
            pointer->fields |=
                    InputDevice::MultiTouchScreenState::Accumulator::FIELD_ABS_MT_WIDTH_MAJOR;
            pointer->absMTWidthMajor = rawEvent->value;
            break;
        case ABS_MT_TRACKING_ID:
            pointer->fields |=
                    InputDevice::MultiTouchScreenState::Accumulator::FIELD_ABS_MT_TRACKING_ID;
            pointer->absMTTrackingId = rawEvent->value;
            break;
        }
    } else if (device->isSingleTouchScreen()) {
        switch (rawEvent->scanCode) {
        case ABS_X:
            device->singleTouchScreen.accumulator.fields |=
                    InputDevice::SingleTouchScreenState::Accumulator::FIELD_ABS_X;
            device->singleTouchScreen.accumulator.absX = rawEvent->value;
            break;
        case ABS_Y:
            device->singleTouchScreen.accumulator.fields |=
                    InputDevice::SingleTouchScreenState::Accumulator::FIELD_ABS_Y;
            device->singleTouchScreen.accumulator.absY = rawEvent->value;
            break;
        case ABS_PRESSURE:
            device->singleTouchScreen.accumulator.fields |=
                    InputDevice::SingleTouchScreenState::Accumulator::FIELD_ABS_PRESSURE;
            device->singleTouchScreen.accumulator.absPressure = rawEvent->value;
            break;
        case ABS_TOOL_WIDTH:
            device->singleTouchScreen.accumulator.fields |=
                    InputDevice::SingleTouchScreenState::Accumulator::FIELD_ABS_TOOL_WIDTH;
            device->singleTouchScreen.accumulator.absToolWidth = rawEvent->value;
            break;
        }
    }
}

void InputReader::handleSwitch(const RawEvent* rawEvent) {
    InputDevice* device = getNonIgnoredDevice(rawEvent->deviceId);
    if (! device) return;

    onSwitch(rawEvent->when, device, rawEvent->scanCode, rawEvent->value);
}

void InputReader::onKey(nsecs_t when, InputDevice* device,
        bool down, int32_t keyCode, int32_t scanCode, uint32_t policyFlags) {
    /* Refresh display properties so we can rotate key codes according to display orientation */

    if (! refreshDisplayProperties()) {
        return;
    }

    /* Update device state */

    int32_t oldMetaState = device->keyboard.current.metaState;
    int32_t newMetaState = updateMetaState(keyCode, down, oldMetaState);
    if (oldMetaState != newMetaState) {
        device->keyboard.current.metaState = newMetaState;
        resetGlobalMetaState();
    }

    // FIXME if we send a down event about a rotated key press we should ensure that we send
    //       a corresponding up event about the rotated key press even if the orientation
    //       has changed in the meantime
    keyCode = rotateKeyCode(keyCode, mDisplayOrientation);

    if (down) {
        device->keyboard.current.downTime = when;
    }

    /* Apply policy */

    int32_t policyActions = mPolicy->interceptKey(when, device->id,
            down, keyCode, scanCode, policyFlags);

    if (! applyStandardInputDispatchPolicyActions(when, policyActions, & policyFlags)) {
        return; // event dropped
    }

    /* Enqueue key event for dispatch */

    int32_t keyEventAction;
    if (down) {
        device->keyboard.current.downTime = when;
        keyEventAction = KEY_EVENT_ACTION_DOWN;
    } else {
        keyEventAction = KEY_EVENT_ACTION_UP;
    }

    int32_t keyEventFlags = KEY_EVENT_FLAG_FROM_SYSTEM;
    if (policyActions & InputReaderPolicyInterface::ACTION_WOKE_HERE) {
        keyEventFlags = keyEventFlags | KEY_EVENT_FLAG_WOKE_HERE;
    }

    mDispatcher->notifyKey(when, device->id, INPUT_EVENT_NATURE_KEY, policyFlags,
            keyEventAction, keyEventFlags, keyCode, scanCode,
            device->keyboard.current.metaState,
            device->keyboard.current.downTime);
}

void InputReader::onSwitch(nsecs_t when, InputDevice* device, int32_t switchCode,
        int32_t switchValue) {
    int32_t policyActions = mPolicy->interceptSwitch(when, switchCode, switchValue);

    uint32_t policyFlags = 0;
    applyStandardInputDispatchPolicyActions(when, policyActions, & policyFlags);
}

void InputReader::onMultiTouchScreenStateChanged(nsecs_t when,
        InputDevice* device) {
    static const uint32_t REQUIRED_FIELDS =
            InputDevice::MultiTouchScreenState::Accumulator::FIELD_ABS_MT_POSITION_X
            | InputDevice::MultiTouchScreenState::Accumulator::FIELD_ABS_MT_POSITION_Y
            | InputDevice::MultiTouchScreenState::Accumulator::FIELD_ABS_MT_TOUCH_MAJOR
            | InputDevice::MultiTouchScreenState::Accumulator::FIELD_ABS_MT_WIDTH_MAJOR;

    /* Refresh display properties so we can map touch screen coords into display coords */

    if (! refreshDisplayProperties()) {
        return;
    }

    /* Update device state */

    InputDevice::MultiTouchScreenState* in = & device->multiTouchScreen;
    InputDevice::TouchData* out = & device->touchScreen.currentTouch;

    uint32_t inCount = in->accumulator.pointerCount;
    uint32_t outCount = 0;
    bool havePointerIds = true;

    out->clear();

    for (uint32_t inIndex = 0; inIndex < inCount; inIndex++) {
        uint32_t fields = in->accumulator.pointers[inIndex].fields;

        if ((fields & REQUIRED_FIELDS) != REQUIRED_FIELDS) {
#if DEBUG_POINTERS
            LOGD("Pointers: Missing required multitouch pointer fields: index=%d, fields=%d",
                    inIndex, fields);
            continue;
#endif
        }

        if (in->accumulator.pointers[inIndex].absMTTouchMajor <= 0) {
            // Pointer is not down.  Drop it.
            continue;
        }

        // FIXME assignment of pressure may be incorrect, probably better to let
        // pressure = touch / width.  Later on we pass width to MotionEvent as a size, which
        // isn't quite right either.  Should be using touch for that.
        out->pointers[outCount].x = in->accumulator.pointers[inIndex].absMTPositionX;
        out->pointers[outCount].y = in->accumulator.pointers[inIndex].absMTPositionY;
        out->pointers[outCount].pressure = in->accumulator.pointers[inIndex].absMTTouchMajor;
        out->pointers[outCount].size = in->accumulator.pointers[inIndex].absMTWidthMajor;

        if (havePointerIds) {
            if (fields & InputDevice::MultiTouchScreenState::Accumulator::
                    FIELD_ABS_MT_TRACKING_ID) {
                uint32_t id = uint32_t(in->accumulator.pointers[inIndex].absMTTrackingId);

                if (id > MAX_POINTER_ID) {
#if DEBUG_POINTERS
                    LOGD("Pointers: Ignoring driver provided pointer id %d because "
                            "it is larger than max supported id %d for optimizations",
                            id, MAX_POINTER_ID);
#endif
                    havePointerIds = false;
                }
                else {
                    out->pointers[outCount].id = id;
                    out->idToIndex[id] = outCount;
                    out->idBits.markBit(id);
                }
            } else {
                havePointerIds = false;
            }
        }

        outCount += 1;
    }

    out->pointerCount = outCount;

    onTouchScreenChanged(when, device, havePointerIds);
}

void InputReader::onSingleTouchScreenStateChanged(nsecs_t when,
        InputDevice* device) {
    /* Refresh display properties so we can map touch screen coords into display coords */

    if (! refreshDisplayProperties()) {
        return;
    }

    /* Update device state */

    InputDevice::SingleTouchScreenState* in = & device->singleTouchScreen;
    InputDevice::TouchData* out = & device->touchScreen.currentTouch;

    uint32_t fields = in->accumulator.fields;

    if (fields & InputDevice::SingleTouchScreenState::Accumulator::FIELD_BTN_TOUCH) {
        in->current.down = in->accumulator.btnTouch;
    }

    if (fields & InputDevice::SingleTouchScreenState::Accumulator::FIELD_ABS_X) {
        in->current.x = in->accumulator.absX;
    }

    if (fields & InputDevice::SingleTouchScreenState::Accumulator::FIELD_ABS_Y) {
        in->current.y = in->accumulator.absY;
    }

    if (fields & InputDevice::SingleTouchScreenState::Accumulator::FIELD_ABS_PRESSURE) {
        in->current.pressure = in->accumulator.absPressure;
    }

    if (fields & InputDevice::SingleTouchScreenState::Accumulator::FIELD_ABS_TOOL_WIDTH) {
        in->current.size = in->accumulator.absToolWidth;
    }

    out->clear();

    if (in->current.down) {
        out->pointerCount = 1;
        out->pointers[0].id = 0;
        out->pointers[0].x = in->current.x;
        out->pointers[0].y = in->current.y;
        out->pointers[0].pressure = in->current.pressure;
        out->pointers[0].size = in->current.size;
        out->idToIndex[0] = 0;
        out->idBits.markBit(0);
    }

    onTouchScreenChanged(when, device, true);
}

void InputReader::onTouchScreenChanged(nsecs_t when,
        InputDevice* device, bool havePointerIds) {
    /* Apply policy */

    int32_t policyActions = mPolicy->interceptTouch(when);

    uint32_t policyFlags = 0;
    if (! applyStandardInputDispatchPolicyActions(when, policyActions, & policyFlags)) {
        device->touchScreen.lastTouch.clear();
        return; // event dropped
    }

    /* Preprocess pointer data */

    if (device->touchScreen.parameters.useBadTouchFilter) {
        if (device->touchScreen.applyBadTouchFilter()) {
            havePointerIds = false;
        }
    }

    if (device->touchScreen.parameters.useJumpyTouchFilter) {
        if (device->touchScreen.applyJumpyTouchFilter()) {
            havePointerIds = false;
        }
    }

    if (! havePointerIds) {
        device->touchScreen.calculatePointerIds();
    }

    InputDevice::TouchData temp;
    InputDevice::TouchData* savedTouch;
    if (device->touchScreen.parameters.useAveragingTouchFilter) {
        temp.copyFrom(device->touchScreen.currentTouch);
        savedTouch = & temp;

        device->touchScreen.applyAveragingTouchFilter();
    } else {
        savedTouch = & device->touchScreen.currentTouch;
    }

    /* Process virtual keys or touches */

    if (! consumeVirtualKeyTouches(when, device, policyFlags)) {
        dispatchTouches(when, device, policyFlags);
    }

    // Copy current touch to last touch in preparation for the next cycle.
    device->touchScreen.lastTouch.copyFrom(*savedTouch);
}

bool InputReader::consumeVirtualKeyTouches(nsecs_t when,
        InputDevice* device, uint32_t policyFlags) {
    switch (device->touchScreen.currentVirtualKey.status) {
    case InputDevice::TouchScreenState::CurrentVirtualKeyState::STATUS_CANCELED:
        if (device->touchScreen.currentTouch.pointerCount == 0) {
            // Pointer went up after virtual key canceled.
            device->touchScreen.currentVirtualKey.status =
                    InputDevice::TouchScreenState::CurrentVirtualKeyState::STATUS_UP;
        }
        return true; // consumed

    case InputDevice::TouchScreenState::CurrentVirtualKeyState::STATUS_DOWN:
        if (device->touchScreen.currentTouch.pointerCount == 0) {
            // Pointer went up while virtual key was down.
            device->touchScreen.currentVirtualKey.status =
                    InputDevice::TouchScreenState::CurrentVirtualKeyState::STATUS_UP;
#if DEBUG_VIRTUAL_KEYS
            LOGD("VirtualKeys: Generating key up: keyCode=%d, scanCode=%d",
                    device->touchScreen.currentVirtualKey.keyCode,
                    device->touchScreen.currentVirtualKey.scanCode);
#endif
            dispatchVirtualKey(when, device, policyFlags, KEY_EVENT_ACTION_UP,
                    KEY_EVENT_FLAG_FROM_SYSTEM | KEY_EVENT_FLAG_VIRTUAL_HARD_KEY);
            return true; // consumed
        }

        if (device->touchScreen.currentTouch.pointerCount == 1) {
            const InputDevice::VirtualKey* virtualKey = device->touchScreen.findVirtualKeyHit();
            if (virtualKey
                    && virtualKey->keyCode == device->touchScreen.currentVirtualKey.keyCode) {
                // Pointer is still within the space of the virtual key.
                return true; // consumed
            }
        }

        // Pointer left virtual key area or another pointer also went down.
        // Send key cancellation.
        device->touchScreen.currentVirtualKey.status =
                InputDevice::TouchScreenState::CurrentVirtualKeyState::STATUS_CANCELED;
#if DEBUG_VIRTUAL_KEYS
        LOGD("VirtualKeys: Canceling key: keyCode=%d, scanCode=%d",
                device->touchScreen.currentVirtualKey.keyCode,
                device->touchScreen.currentVirtualKey.scanCode);
#endif
        dispatchVirtualKey(when, device, policyFlags, KEY_EVENT_ACTION_UP,
                KEY_EVENT_FLAG_FROM_SYSTEM | KEY_EVENT_FLAG_VIRTUAL_HARD_KEY
                        | KEY_EVENT_FLAG_CANCELED);
        return true; // consumed

    default:
        if (device->touchScreen.currentTouch.pointerCount == 1
                && device->touchScreen.lastTouch.pointerCount == 0) {
            // Pointer just went down.  Check for virtual key hit.
            const InputDevice::VirtualKey* virtualKey = device->touchScreen.findVirtualKeyHit();
            if (virtualKey) {
                device->touchScreen.currentVirtualKey.status =
                        InputDevice::TouchScreenState::CurrentVirtualKeyState::STATUS_DOWN;
                device->touchScreen.currentVirtualKey.downTime = when;
                device->touchScreen.currentVirtualKey.keyCode = virtualKey->keyCode;
                device->touchScreen.currentVirtualKey.scanCode = virtualKey->scanCode;
#if DEBUG_VIRTUAL_KEYS
                LOGD("VirtualKeys: Generating key down: keyCode=%d, scanCode=%d",
                        device->touchScreen.currentVirtualKey.keyCode,
                        device->touchScreen.currentVirtualKey.scanCode);
#endif
                dispatchVirtualKey(when, device, policyFlags, KEY_EVENT_ACTION_DOWN,
                        KEY_EVENT_FLAG_FROM_SYSTEM | KEY_EVENT_FLAG_VIRTUAL_HARD_KEY);
                return true; // consumed
            }
        }
        return false; // not consumed
    }
}

void InputReader::dispatchVirtualKey(nsecs_t when,
        InputDevice* device, uint32_t policyFlags,
        int32_t keyEventAction, int32_t keyEventFlags) {
    updateExportedVirtualKeyState();

    int32_t keyCode = device->touchScreen.currentVirtualKey.keyCode;
    int32_t scanCode = device->touchScreen.currentVirtualKey.scanCode;
    nsecs_t downTime = device->touchScreen.currentVirtualKey.downTime;
    int32_t metaState = globalMetaState();

    if (keyEventAction == KEY_EVENT_ACTION_DOWN) {
        mPolicy->virtualKeyDownFeedback();
    }

    int32_t policyActions = mPolicy->interceptKey(when, device->id,
            keyEventAction == KEY_EVENT_ACTION_DOWN, keyCode, scanCode, policyFlags);

    if (applyStandardInputDispatchPolicyActions(when, policyActions, & policyFlags)) {
        mDispatcher->notifyKey(when, device->id, INPUT_EVENT_NATURE_KEY, policyFlags,
                keyEventAction, keyEventFlags, keyCode, scanCode, metaState, downTime);
    }
}

void InputReader::dispatchTouches(nsecs_t when,
        InputDevice* device, uint32_t policyFlags) {
    uint32_t currentPointerCount = device->touchScreen.currentTouch.pointerCount;
    uint32_t lastPointerCount = device->touchScreen.lastTouch.pointerCount;
    if (currentPointerCount == 0 && lastPointerCount == 0) {
        return; // nothing to do!
    }

    BitSet32 currentIdBits = device->touchScreen.currentTouch.idBits;
    BitSet32 lastIdBits = device->touchScreen.lastTouch.idBits;

    if (currentIdBits == lastIdBits) {
        // No pointer id changes so this is a move event.
        // The dispatcher takes care of batching moves so we don't have to deal with that here.
        int32_t motionEventAction = MOTION_EVENT_ACTION_MOVE;
        dispatchTouch(when, device, policyFlags, & device->touchScreen.currentTouch,
                currentIdBits, motionEventAction);
    } else {
        // There may be pointers going up and pointers going down at the same time when pointer
        // ids are reported by the device driver.
        BitSet32 upIdBits(lastIdBits.value & ~ currentIdBits.value);
        BitSet32 downIdBits(currentIdBits.value & ~ lastIdBits.value);
        BitSet32 activeIdBits(lastIdBits.value);

        while (! upIdBits.isEmpty()) {
            uint32_t upId = upIdBits.firstMarkedBit();
            upIdBits.clearBit(upId);
            BitSet32 oldActiveIdBits = activeIdBits;
            activeIdBits.clearBit(upId);

            int32_t motionEventAction;
            if (activeIdBits.isEmpty()) {
                motionEventAction = MOTION_EVENT_ACTION_UP;
            } else {
                motionEventAction = MOTION_EVENT_ACTION_POINTER_UP
                        | (upId << MOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
            }

            dispatchTouch(when, device, policyFlags, & device->touchScreen.lastTouch,
                    oldActiveIdBits, motionEventAction);
        }

        while (! downIdBits.isEmpty()) {
            uint32_t downId = downIdBits.firstMarkedBit();
            downIdBits.clearBit(downId);
            BitSet32 oldActiveIdBits = activeIdBits;
            activeIdBits.markBit(downId);

            int32_t motionEventAction;
            if (oldActiveIdBits.isEmpty()) {
                motionEventAction = MOTION_EVENT_ACTION_DOWN;
                device->touchScreen.downTime = when;
            } else {
                motionEventAction = MOTION_EVENT_ACTION_POINTER_DOWN
                        | (downId << MOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
            }

            dispatchTouch(when, device, policyFlags, & device->touchScreen.currentTouch,
                    activeIdBits, motionEventAction);
        }
    }
}

void InputReader::dispatchTouch(nsecs_t when, InputDevice* device, uint32_t policyFlags,
        InputDevice::TouchData* touch, BitSet32 idBits,
        int32_t motionEventAction) {
    int32_t orientedWidth, orientedHeight;
    switch (mDisplayOrientation) {
    case InputReaderPolicyInterface::ROTATION_90:
    case InputReaderPolicyInterface::ROTATION_270:
        orientedWidth = mDisplayHeight;
        orientedHeight = mDisplayWidth;
        break;
    default:
        orientedWidth = mDisplayWidth;
        orientedHeight = mDisplayHeight;
        break;
    }

    uint32_t pointerCount = 0;
    int32_t pointerIds[MAX_POINTERS];
    PointerCoords pointerCoords[MAX_POINTERS];

    const InputDevice::TouchScreenState::Precalculated& precalculated =
            device->touchScreen.precalculated;

    // Walk through the the active pointers and map touch screen coordinates (TouchData) into
    // display coordinates (PointerCoords) and adjust for display orientation.
    while (! idBits.isEmpty()) {
        uint32_t id = idBits.firstMarkedBit();
        idBits.clearBit(id);
        uint32_t index = touch->idToIndex[id];

        float x = float(touch->pointers[index].x
                - precalculated.xOrigin) * precalculated.xScale;
        float y = float(touch->pointers[index].y
                - precalculated.yOrigin) * precalculated.yScale;
        float pressure = float(touch->pointers[index].pressure
                - precalculated.pressureOrigin) * precalculated.pressureScale;
        float size = float(touch->pointers[index].size
                - precalculated.sizeOrigin) * precalculated.sizeScale;

        switch (mDisplayOrientation) {
        case InputReaderPolicyInterface::ROTATION_90: {
            float xTemp = x;
            x = y;
            y = mDisplayWidth - xTemp;
            break;
        }
        case InputReaderPolicyInterface::ROTATION_180: {
            x = mDisplayWidth - x;
            y = mDisplayHeight - y;
            break;
        }
        case InputReaderPolicyInterface::ROTATION_270: {
            float xTemp = x;
            x = mDisplayHeight - y;
            y = xTemp;
            break;
        }
        }

        pointerIds[pointerCount] = int32_t(id);

        pointerCoords[pointerCount].x = x;
        pointerCoords[pointerCount].y = y;
        pointerCoords[pointerCount].pressure = pressure;
        pointerCoords[pointerCount].size = size;

        pointerCount += 1;
    }

    // Check edge flags by looking only at the first pointer since the flags are
    // global to the event.
    // XXX Maybe we should revise the edge flags API to work on a per-pointer basis.
    int32_t motionEventEdgeFlags = 0;
    if (motionEventAction == MOTION_EVENT_ACTION_DOWN) {
        if (pointerCoords[0].x <= 0) {
            motionEventEdgeFlags |= MOTION_EVENT_EDGE_FLAG_LEFT;
        } else if (pointerCoords[0].x >= orientedWidth) {
            motionEventEdgeFlags |= MOTION_EVENT_EDGE_FLAG_RIGHT;
        }
        if (pointerCoords[0].y <= 0) {
            motionEventEdgeFlags |= MOTION_EVENT_EDGE_FLAG_TOP;
        } else if (pointerCoords[0].y >= orientedHeight) {
            motionEventEdgeFlags |= MOTION_EVENT_EDGE_FLAG_BOTTOM;
        }
    }

    nsecs_t downTime = device->touchScreen.downTime;
    mDispatcher->notifyMotion(when, device->id, INPUT_EVENT_NATURE_TOUCH, policyFlags,
            motionEventAction, globalMetaState(), motionEventEdgeFlags,
            pointerCount, pointerIds, pointerCoords,
            0, 0, downTime);
}

void InputReader::onTrackballStateChanged(nsecs_t when,
        InputDevice* device) {
    static const uint32_t DELTA_FIELDS =
            InputDevice::TrackballState::Accumulator::FIELD_REL_X
            | InputDevice::TrackballState::Accumulator::FIELD_REL_Y;

    /* Refresh display properties so we can trackball moves according to display orientation */

    if (! refreshDisplayProperties()) {
        return;
    }

    /* Update device state */

    uint32_t fields = device->trackball.accumulator.fields;
    bool downChanged = fields & InputDevice::TrackballState::Accumulator::FIELD_BTN_MOUSE;
    bool deltaChanged = fields & DELTA_FIELDS;

    bool down;
    if (downChanged) {
        if (device->trackball.accumulator.btnMouse) {
            device->trackball.current.down = true;
            device->trackball.current.downTime = when;
            down = true;
        } else {
            device->trackball.current.down = false;
            down = false;
        }
    } else {
        down = device->trackball.current.down;
    }

    /* Apply policy */

    int32_t policyActions = mPolicy->interceptTrackball(when, downChanged, down, deltaChanged);

    uint32_t policyFlags = 0;
    if (! applyStandardInputDispatchPolicyActions(when, policyActions, & policyFlags)) {
        return; // event dropped
    }

    /* Enqueue motion event for dispatch */

    int32_t motionEventAction;
    if (downChanged) {
        motionEventAction = down ? MOTION_EVENT_ACTION_DOWN : MOTION_EVENT_ACTION_UP;
    } else {
        motionEventAction = MOTION_EVENT_ACTION_MOVE;
    }

    int32_t pointerId = 0;
    PointerCoords pointerCoords;
    pointerCoords.x = fields & InputDevice::TrackballState::Accumulator::FIELD_REL_X
            ? device->trackball.accumulator.relX * device->trackball.precalculated.xScale : 0;
    pointerCoords.y = fields & InputDevice::TrackballState::Accumulator::FIELD_REL_Y
            ? device->trackball.accumulator.relY * device->trackball.precalculated.yScale : 0;
    pointerCoords.pressure = 1.0f; // XXX Consider making this 1.0f if down, 0 otherwise.
    pointerCoords.size = 0;

    float temp;
    switch (mDisplayOrientation) {
    case InputReaderPolicyInterface::ROTATION_90:
        temp = pointerCoords.x;
        pointerCoords.x = pointerCoords.y;
        pointerCoords.y = - temp;
        break;

    case InputReaderPolicyInterface::ROTATION_180:
        pointerCoords.x = - pointerCoords.x;
        pointerCoords.y = - pointerCoords.y;
        break;

    case InputReaderPolicyInterface::ROTATION_270:
        temp = pointerCoords.x;
        pointerCoords.x = - pointerCoords.y;
        pointerCoords.y = temp;
        break;
    }

    mDispatcher->notifyMotion(when, device->id, INPUT_EVENT_NATURE_TRACKBALL, policyFlags,
            motionEventAction, globalMetaState(), MOTION_EVENT_EDGE_FLAG_NONE,
            1, & pointerId, & pointerCoords,
            device->trackball.precalculated.xPrecision,
            device->trackball.precalculated.yPrecision,
            device->trackball.current.downTime);
}

void InputReader::onConfigurationChanged(nsecs_t when) {
    // Reset global meta state because it depends on the list of all configured devices.
    resetGlobalMetaState();

    // Reset virtual keys, just in case.
    updateExportedVirtualKeyState();

    // Update input configuration.
    updateExportedInputConfiguration();

    // Enqueue configuration changed.
    mDispatcher->notifyConfigurationChanged(when);
}

bool InputReader::applyStandardInputDispatchPolicyActions(nsecs_t when,
        int32_t policyActions, uint32_t* policyFlags) {
    if (policyActions & InputReaderPolicyInterface::ACTION_APP_SWITCH_COMING) {
        mDispatcher->notifyAppSwitchComing(when);
    }

    if (policyActions & InputReaderPolicyInterface::ACTION_WOKE_HERE) {
        *policyFlags |= POLICY_FLAG_WOKE_HERE;
    }

    if (policyActions & InputReaderPolicyInterface::ACTION_BRIGHT_HERE) {
        *policyFlags |= POLICY_FLAG_BRIGHT_HERE;
    }

    return policyActions & InputReaderPolicyInterface::ACTION_DISPATCH;
}

void InputReader::resetDisplayProperties() {
    mDisplayWidth = mDisplayHeight = -1;
    mDisplayOrientation = -1;
}

bool InputReader::refreshDisplayProperties() {
    int32_t newWidth, newHeight, newOrientation;
    if (mPolicy->getDisplayInfo(0, & newWidth, & newHeight, & newOrientation)) {
        if (newWidth != mDisplayWidth || newHeight != mDisplayHeight) {
            LOGD("Display size changed from %dx%d to %dx%d, updating device configuration",
                    mDisplayWidth, mDisplayHeight, newWidth, newHeight);

            mDisplayWidth = newWidth;
            mDisplayHeight = newHeight;

            for (size_t i = 0; i < mDevices.size(); i++) {
                configureDeviceForCurrentDisplaySize(mDevices.valueAt(i));
            }
        }

        if (newOrientation != mDisplayOrientation) {
            LOGD("Display orientation changed to %d", mDisplayOrientation);

            mDisplayOrientation = newOrientation;
        }
        return true;
    } else {
        resetDisplayProperties();
        return false;
    }
}

InputDevice* InputReader::getDevice(int32_t deviceId) {
    ssize_t index = mDevices.indexOfKey(deviceId);
    return index >= 0 ? mDevices.valueAt((size_t) index) : NULL;
}

InputDevice* InputReader::getNonIgnoredDevice(int32_t deviceId) {
    InputDevice* device = getDevice(deviceId);
    return device && ! device->ignored ? device : NULL;
}

void InputReader::addDevice(nsecs_t when, int32_t deviceId) {
    uint32_t classes = mEventHub->getDeviceClasses(deviceId);
    String8 name = mEventHub->getDeviceName(deviceId);
    InputDevice* device = new InputDevice(deviceId, classes, name);

    if (classes != 0) {
        LOGI("Device added: id=0x%x, name=%s, classes=%02x", device->id,
                device->name.string(), device->classes);

        configureDevice(device);
    } else {
        LOGI("Device added: id=0x%x, name=%s (ignored non-input device)", device->id,
                device->name.string());

        device->ignored = true;
    }

    device->reset();

    mDevices.add(deviceId, device);

    if (! device->ignored) {
        onConfigurationChanged(when);
    }
}

void InputReader::removeDevice(nsecs_t when, InputDevice* device) {
    mDevices.removeItem(device->id);

    if (! device->ignored) {
        LOGI("Device removed: id=0x%x, name=%s, classes=%02x", device->id,
                device->name.string(), device->classes);

        onConfigurationChanged(when);
    } else {
        LOGI("Device removed: id=0x%x, name=%s (ignored non-input device)", device->id,
                device->name.string());
    }

    delete device;
}

void InputReader::configureDevice(InputDevice* device) {
    if (device->isMultiTouchScreen()) {
        configureAbsoluteAxisInfo(device, ABS_MT_POSITION_X, "X",
                & device->touchScreen.parameters.xAxis);
        configureAbsoluteAxisInfo(device, ABS_MT_POSITION_Y, "Y",
                & device->touchScreen.parameters.yAxis);
        configureAbsoluteAxisInfo(device, ABS_MT_TOUCH_MAJOR, "Pressure",
                & device->touchScreen.parameters.pressureAxis);
        configureAbsoluteAxisInfo(device, ABS_MT_WIDTH_MAJOR, "Size",
                & device->touchScreen.parameters.sizeAxis);
    } else if (device->isSingleTouchScreen()) {
        configureAbsoluteAxisInfo(device, ABS_X, "X",
                & device->touchScreen.parameters.xAxis);
        configureAbsoluteAxisInfo(device, ABS_Y, "Y",
                & device->touchScreen.parameters.yAxis);
        configureAbsoluteAxisInfo(device, ABS_PRESSURE, "Pressure",
                & device->touchScreen.parameters.pressureAxis);
        configureAbsoluteAxisInfo(device, ABS_TOOL_WIDTH, "Size",
                & device->touchScreen.parameters.sizeAxis);
    }

    if (device->isTouchScreen()) {
        device->touchScreen.parameters.useBadTouchFilter =
                mPolicy->filterTouchEvents();
        device->touchScreen.parameters.useAveragingTouchFilter =
                mPolicy->filterTouchEvents();
        device->touchScreen.parameters.useJumpyTouchFilter =
                mPolicy->filterJumpyTouchEvents();

        if (device->touchScreen.parameters.pressureAxis.valid) {
            device->touchScreen.precalculated.pressureOrigin =
                    device->touchScreen.parameters.pressureAxis.minValue;
            device->touchScreen.precalculated.pressureScale =
                    1.0f / device->touchScreen.parameters.pressureAxis.range;
        } else {
            device->touchScreen.precalculated.pressureOrigin = 0;
            device->touchScreen.precalculated.pressureScale = 1.0f;
        }

        if (device->touchScreen.parameters.sizeAxis.valid) {
            device->touchScreen.precalculated.sizeOrigin =
                    device->touchScreen.parameters.sizeAxis.minValue;
            device->touchScreen.precalculated.sizeScale =
                    1.0f / device->touchScreen.parameters.sizeAxis.range;
        } else {
            device->touchScreen.precalculated.sizeOrigin = 0;
            device->touchScreen.precalculated.sizeScale = 1.0f;
        }
    }

    if (device->isTrackball()) {
        device->trackball.precalculated.xPrecision = TRACKBALL_MOVEMENT_THRESHOLD;
        device->trackball.precalculated.yPrecision = TRACKBALL_MOVEMENT_THRESHOLD;
        device->trackball.precalculated.xScale = 1.0f / TRACKBALL_MOVEMENT_THRESHOLD;
        device->trackball.precalculated.yScale = 1.0f / TRACKBALL_MOVEMENT_THRESHOLD;
    }

    configureDeviceForCurrentDisplaySize(device);
}

void InputReader::configureDeviceForCurrentDisplaySize(InputDevice* device) {
    if (device->isTouchScreen()) {
        if (device->touchScreen.parameters.xAxis.valid
                && device->touchScreen.parameters.yAxis.valid) {
            device->touchScreen.precalculated.xOrigin =
                    device->touchScreen.parameters.xAxis.minValue;
            device->touchScreen.precalculated.yOrigin =
                    device->touchScreen.parameters.yAxis.minValue;

            if (mDisplayWidth < 0) {
                LOGD("Skipping part of touch screen configuration since display size is unknown.");

                device->touchScreen.precalculated.xScale = 1.0f;
                device->touchScreen.precalculated.yScale = 1.0f;
            } else {
                LOGI("Device configured: id=0x%x, name=%s (display size was changed)", device->id,
                        device->name.string());

                device->touchScreen.precalculated.xScale =
                        float(mDisplayWidth) / device->touchScreen.parameters.xAxis.range;
                device->touchScreen.precalculated.yScale =
                        float(mDisplayHeight) / device->touchScreen.parameters.yAxis.range;

                configureVirtualKeys(device);
            }
        } else {
            device->touchScreen.precalculated.xOrigin = 0;
            device->touchScreen.precalculated.xScale = 1.0f;
            device->touchScreen.precalculated.yOrigin = 0;
            device->touchScreen.precalculated.yScale = 1.0f;
        }
    }
}

void InputReader::configureVirtualKeys(InputDevice* device) {
    assert(device->touchScreen.parameters.xAxis.valid
            && device->touchScreen.parameters.yAxis.valid);

    device->touchScreen.virtualKeys.clear();

    Vector<InputReaderPolicyInterface::VirtualKeyDefinition> virtualKeyDefinitions;
    mPolicy->getVirtualKeyDefinitions(device->name, virtualKeyDefinitions);
    if (virtualKeyDefinitions.size() == 0) {
        return;
    }

    device->touchScreen.virtualKeys.setCapacity(virtualKeyDefinitions.size());

    int32_t touchScreenLeft = device->touchScreen.parameters.xAxis.minValue;
    int32_t touchScreenTop = device->touchScreen.parameters.yAxis.minValue;
    int32_t touchScreenWidth = device->touchScreen.parameters.xAxis.range;
    int32_t touchScreenHeight = device->touchScreen.parameters.yAxis.range;

    for (size_t i = 0; i < virtualKeyDefinitions.size(); i++) {
        const InputReaderPolicyInterface::VirtualKeyDefinition& virtualKeyDefinition =
                virtualKeyDefinitions[i];

        device->touchScreen.virtualKeys.add();
        InputDevice::VirtualKey& virtualKey =
                device->touchScreen.virtualKeys.editTop();

        virtualKey.scanCode = virtualKeyDefinition.scanCode;
        int32_t keyCode;
        uint32_t flags;
        if (mEventHub->scancodeToKeycode(device->id, virtualKey.scanCode,
                & keyCode, & flags)) {
            LOGW("  VirtualKey %d: could not obtain key code, ignoring", virtualKey.scanCode);
            device->touchScreen.virtualKeys.pop(); // drop the key
            continue;
        }

        virtualKey.keyCode = keyCode;
        virtualKey.flags = flags;

        // convert the key definition's display coordinates into touch coordinates for a hit box
        int32_t halfWidth = virtualKeyDefinition.width / 2;
        int32_t halfHeight = virtualKeyDefinition.height / 2;

        virtualKey.hitLeft = (virtualKeyDefinition.centerX - halfWidth)
                * touchScreenWidth / mDisplayWidth + touchScreenLeft;
        virtualKey.hitRight= (virtualKeyDefinition.centerX + halfWidth)
                * touchScreenWidth / mDisplayWidth + touchScreenLeft;
        virtualKey.hitTop = (virtualKeyDefinition.centerY - halfHeight)
                * touchScreenHeight / mDisplayHeight + touchScreenTop;
        virtualKey.hitBottom = (virtualKeyDefinition.centerY + halfHeight)
                * touchScreenHeight / mDisplayHeight + touchScreenTop;

        LOGI("  VirtualKey %d: keyCode=%d hitLeft=%d hitRight=%d hitTop=%d hitBottom=%d",
                virtualKey.scanCode, virtualKey.keyCode,
                virtualKey.hitLeft, virtualKey.hitRight, virtualKey.hitTop, virtualKey.hitBottom);
    }
}

void InputReader::configureAbsoluteAxisInfo(InputDevice* device,
        int axis, const char* name, InputDevice::AbsoluteAxisInfo* out) {
    if (! mEventHub->getAbsoluteInfo(device->id, axis,
            & out->minValue, & out->maxValue, & out->flat, &out->fuzz)) {
        out->range = out->maxValue - out->minValue;
        if (out->range != 0) {
            LOGI("  %s: min=%d max=%d flat=%d fuzz=%d",
                    name, out->minValue, out->maxValue, out->flat, out->fuzz);
            out->valid = true;
            return;
        }
    }

    out->valid = false;
    out->minValue = 0;
    out->maxValue = 0;
    out->flat = 0;
    out->fuzz = 0;
    out->range = 0;
    LOGI("  %s: unknown axis values, marking as invalid", name);
}

void InputReader::configureExcludedDevices() {
    Vector<String8> excludedDeviceNames;
    mPolicy->getExcludedDeviceNames(excludedDeviceNames);

    for (size_t i = 0; i < excludedDeviceNames.size(); i++) {
        mEventHub->addExcludedDevice(excludedDeviceNames[i]);
    }
}

void InputReader::resetGlobalMetaState() {
    mGlobalMetaState = -1;
}

int32_t InputReader::globalMetaState() {
    if (mGlobalMetaState == -1) {
        mGlobalMetaState = 0;
        for (size_t i = 0; i < mDevices.size(); i++) {
            InputDevice* device = mDevices.valueAt(i);
            if (device->isKeyboard()) {
                mGlobalMetaState |= device->keyboard.current.metaState;
            }
        }
    }
    return mGlobalMetaState;
}

void InputReader::updateExportedVirtualKeyState() {
    int32_t keyCode = -1, scanCode = -1;

    for (size_t i = 0; i < mDevices.size(); i++) {
        InputDevice* device = mDevices.valueAt(i);
        if (device->isTouchScreen()) {
            if (device->touchScreen.currentVirtualKey.status
                    == InputDevice::TouchScreenState::CurrentVirtualKeyState::STATUS_DOWN) {
                keyCode = device->touchScreen.currentVirtualKey.keyCode;
                scanCode = device->touchScreen.currentVirtualKey.scanCode;
            }
        }
    }

    { // acquire exported state lock
        AutoMutex _l(mExportedStateLock);

        mExportedVirtualKeyCode = keyCode;
        mExportedVirtualScanCode = scanCode;
    } // release exported state lock
}

bool InputReader::getCurrentVirtualKey(int32_t* outKeyCode, int32_t* outScanCode) const {
    { // acquire exported state lock
        AutoMutex _l(mExportedStateLock);

        *outKeyCode = mExportedVirtualKeyCode;
        *outScanCode = mExportedVirtualScanCode;
        return mExportedVirtualKeyCode != -1;
    } // release exported state lock
}

void InputReader::updateExportedInputConfiguration() {
    int32_t touchScreenConfig = InputConfiguration::TOUCHSCREEN_NOTOUCH;
    int32_t keyboardConfig = InputConfiguration::KEYBOARD_NOKEYS;
    int32_t navigationConfig = InputConfiguration::NAVIGATION_NONAV;

    for (size_t i = 0; i < mDevices.size(); i++) {
        InputDevice* device = mDevices.valueAt(i);
        int32_t deviceClasses = device->classes;

        if (deviceClasses & INPUT_DEVICE_CLASS_TOUCHSCREEN) {
            touchScreenConfig = InputConfiguration::TOUCHSCREEN_FINGER;
        }
        if (deviceClasses & INPUT_DEVICE_CLASS_ALPHAKEY) {
            keyboardConfig = InputConfiguration::KEYBOARD_QWERTY;
        }
        if (deviceClasses & INPUT_DEVICE_CLASS_TRACKBALL) {
            navigationConfig = InputConfiguration::NAVIGATION_TRACKBALL;
        } else if (deviceClasses & INPUT_DEVICE_CLASS_DPAD) {
            navigationConfig = InputConfiguration::NAVIGATION_DPAD;
        }
    }

    { // acquire exported state lock
        AutoMutex _l(mExportedStateLock);

        mExportedInputConfiguration.touchScreen = touchScreenConfig;
        mExportedInputConfiguration.keyboard = keyboardConfig;
        mExportedInputConfiguration.navigation = navigationConfig;
    } // release exported state lock
}

void InputReader::getCurrentInputConfiguration(InputConfiguration* outConfiguration) const {
    { // acquire exported state lock
        AutoMutex _l(mExportedStateLock);

        *outConfiguration = mExportedInputConfiguration;
    } // release exported state lock
}

int32_t InputReader::getCurrentScanCodeState(int32_t deviceId, int32_t deviceClasses,
        int32_t scanCode) const {
    { // acquire exported state lock
        AutoMutex _l(mExportedStateLock);

        if (mExportedVirtualScanCode == scanCode) {
            return KEY_STATE_VIRTUAL;
        }
    } // release exported state lock

    return mEventHub->getScanCodeState(deviceId, deviceClasses, scanCode);
}

int32_t InputReader::getCurrentKeyCodeState(int32_t deviceId, int32_t deviceClasses,
        int32_t keyCode) const {
    { // acquire exported state lock
        AutoMutex _l(mExportedStateLock);

        if (mExportedVirtualKeyCode == keyCode) {
            return KEY_STATE_VIRTUAL;
        }
    } // release exported state lock

    return mEventHub->getKeyCodeState(deviceId, deviceClasses, keyCode);
}

int32_t InputReader::getCurrentSwitchState(int32_t deviceId, int32_t deviceClasses,
        int32_t sw) const {
    return mEventHub->getSwitchState(deviceId, deviceClasses, sw);
}

bool InputReader::hasKeys(size_t numCodes, const int32_t* keyCodes, uint8_t* outFlags) const {
    return mEventHub->hasKeys(numCodes, keyCodes, outFlags);
}


// --- InputReaderThread ---

InputReaderThread::InputReaderThread(const sp<InputReaderInterface>& reader) :
        Thread(/*canCallJava*/ true), mReader(reader) {
}

InputReaderThread::~InputReaderThread() {
}

bool InputReaderThread::threadLoop() {
    mReader->loopOnce();
    return true;
}

} // namespace android