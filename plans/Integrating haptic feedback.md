Integrating haptic feedback into Gladius transforms the abstract mathematical nodes and invisible distance fields into tactile, physical sensations.
Because you are using a raymarching engine, you possess a massive architectural advantage: you have a mathematical description of density, distance, and surface normals for everything in your workspace.
By tapping into the Steam Frame Controllers' linear resonant actuators (LRAs) via OpenXR (xrApplyHapticFeedback), you can convert these mathematical gradients directly into physical vibrations.
Here are the four most unique ways to integrate haptic feedback into the Gladius spatial workflow:
## 1. The SDF Surface "Detent" (Tactile Ray Proximity)
When slicing or sculpting an implicit model, it’s hard to feel exactly where a clipping plane or an additive tool tip makes contact with the surface.

* The Technique: Track the distance value (d) from your node graph at the exact 3D tip of your right controller stylus. As the stylus gets closer to the model (d approaches 0), fire a low-frequency, ticking pulse that grows tighter and faster.
* The Sensation: It acts like a high-end automotive parking sensor or a physical magnetic detent, giving you a tactile sense of surface proximity before you even click a trigger to start modeling.

------------------------------
## 2. Snap-to-Socket Resistance (Magnetic UI Snapping)
In section 4.3 of your design document, we mapped out a system where connecting node wires "magnetically" snap to valid input ports. Haptics make this feel incredibly satisfying.

* The Technique: When dragging a connection wire with your right controller, monitor the distance to the nearest valid input port. The moment the laser ray gets close enough and the wire bends to snap onto the target socket, trigger a single, sharp high-amplitude, short-duration pop (e.g., 100% amplitude for 10 milliseconds).
* The Sensation: It feels like a physical magnet snapping a heavy metal plug into a socket, giving you instant confirmation that the node link was successfully sealed without needing a visual text pop-up.

------------------------------
## 3. The Functional Pulse (Data Flow Vibrations)
Since your 3D spatial graph visualizes real-time data moving down the capsule wires, you can let the user physically feel the math operating inside a branch.

* The Technique: When the right controller pointer touches or intersects a 3D node or connection tube, read the absolute value or frequency of the mathematical function passing through that specific edge.
* The Sensation: If a branch is pushing a high-frequency sine wave, the controller will hum with a smooth, high-pitched vibration. If it’s passing a heavy, low-frequency square wave, the controller will thud at a slower, heavier rhythmic pace. You can literally troubleshoot your node architecture by blindly touching the wires to feel which branches are active.

------------------------------
## 4. Slider Friction & Depth Gearing
When using the Spatial Depth-Clutch to change numeric values, your hands are moving through thin air. Haptics supply the missing resistance.

* The Technique: As you drag your hand left or right to change a value, pulse the haptic motor every time the number crosses an integer threshold (e.g., passing 1.0, 2.0, 3.0).
* The Dynamic Twist: Tie the vibration frequency to the depth clutch.
* When your hand is close to the canvas (Macro Mode), the clicks are heavy and spaced apart, like a heavy industrial ratchet wheel.
   * When you pull your hand back (Micro Mode), the clicks turn into an incredibly fine, high-frequency purr, mimicking the sensation of a high-precision mechanical watch gear turning between your fingers.

------------------------------
## 💻 OpenXR Implementation C++ Snippet
To send these custom waveforms to the Steam Frame controllers, you populate the XrHapticVibration struct and apply it to the active interaction path:

void ApplySpatialHapticFeedback(XrSession session, XrAction haptic_action, XrPath hand_path, float amplitude, float frequency, float duration_seconds) {
    XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};
    vibration.next = nullptr;
    vibration.amplitude = std::clamp(amplitude, 0.0f, 1.0f);
    vibration.frequency = frequency; // e.g., 300.0f Hz for a sharp click, 60.0f Hz for a heavy thud
    vibration.duration = XR_INFINITE_DURATION; // Or cast a specific duration via std::chrono

    XrHapticActionInfo action_info{XR_TYPE_HAPTIC_ACTION_INFO};
    action_info.next = nullptr;
    action_info.action = haptic_action;
    action_info.subactionPath = hand_path;

    xrApplyHapticFeedback(session, &action_info, reinterpret_cast<const XrHapticBaseHeader*>(&vibration));
}
