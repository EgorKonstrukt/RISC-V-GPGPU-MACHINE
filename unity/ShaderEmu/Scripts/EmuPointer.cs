using UdonSharp;
using UnityEngine;
using TMPro;
using VRC.SDKBase;
using VRC.Udon.Common;

// The players' beams: one per hand in VR, each drawn as a line with a dot where it lands, and
// the middle of the view on desktop. A beam on the display is the machine's pointer
// (docs/input.md): trigger or click is the left button, grip or right click the right one.
// A beam on a keyboard holds the key under it while the trigger is held; both hands type.
[UdonBehaviourSyncMode(BehaviourSyncMode.None)]
public class EmuPointer : UdonSharpBehaviour
{
    public EmuMachine machine;
    public Collider screen;            // on the display quad (a unit quad, scaled)
    public float aspect = 1.7777778f;  // the quad's width / height, as Display.shader has it
    public float maxDistance = 12f;
    public EmuKeyboard[] keyboards;
    public Collider[] keyboardPlates;  // one a keyboard, same order
    public LineRenderer[] beams;       // left hand, right hand
    public Transform[] dots;
    public Renderer[] dotRenderers;
    public Material faintMaterial;     // a beam that only points
    public Material solidMaterial;     // a beam whose trigger or grip is held
    public float pitch;                // degrees the beam is tipped towards the palm
    public TextMeshProUGUI pitchLabel;
    // The classroom (EmuStations): a keyboard and a tube a place. Only this visitor's own answer.
    public Collider[] tubes;           // a unit quad over each tube's glass
    public float tubeAspect = 1.3333334f;
    public float tubeBulge = 1.8f;     // the glass is part of a ball of this radius (world/pc.py's BULGE)
    public float tubeGap = 0.024f;     // from a tube's plate to the middle of its glass
    public float tubeFill = 1f;        // how much of the glass the picture spans (CRT.shader's _Fill)
    public int stationKeysFrom = -1;   // where the classroom's keyboards start in `keyboards`
    public int holoKeyboard = -1;      // the holodeck's keyboard in `keyboards`: a sitter's beams press it too
    // The holodecks (EmuHolodeck): a screen a room, a unit quad. Only the sitter's own answers.
    public Collider[] holoScreens;
    public float holoAspect = 1.3333334f;
    [HideInInspector] public int ownHolo = -1;
    [HideInInspector] public int ownStation = -1;
    [HideInInspector] public bool ownDisplay;   // its tube shows the display, not the console
    // The game controller is in the player's hands (EmuGamepad): the beams answer only the
    // console beside their holodeck's seat.
    [HideInInspector] public bool away;
    // A holodeck's console to a beam: a plate a room, its keys as x, y, width, height in shares
    // of the plate from its top left, and the event of EmuHolodeck each sends. Only a sitter in
    // a headset presses them this way (holoKeys); anybody else's are VRChat's own laser's.
    public Collider[] holoPlates;
    public float[] holoKeyRects;
    public string[] holoKeyEvents;
    public UdonSharpBehaviour holodeck;
    public Transform holoMark;         // over the key a beam is on
    [HideInInspector] public bool holoKeys;
    // The small button that brings a console back, a room: the last of holoKeyEvents is its event.
    public Collider[] holoShow;
    // The sitter's program shows a flat screen (a menu): their beams are its pointer on the room's screen.
    [HideInInspector] public bool holoFlat;
    // The sitter put the console away: the room's screen answers no beam then.
    [HideInInspector] public bool holoHidden;
    // Where each hand's beam starts and the way it goes, in a headset (EmuHolodeck tells the machine)
    [HideInInspector] public Vector3[] handAt = new Vector3[2], handWay = new Vector3[2];
    [HideInInspector] public bool handsKnown;
    // A sitter's aim in their program's world: a beam a hand (HandBeam.shader), from where and
    // the way the machine is told that hand points. aimHands: bit 0 the left, bit 1 the right.
    public Transform[] aimBeams;
    [HideInInspector] public int aimHands;
    // A test's alone, set from editor code in play mode: hands that stand before the head, as
    // if tracked. Kept in no scene and set by nothing here: a sitter without a headset has no beam.
    [System.NonSerialized] public bool debugBeam;
    // The danger zone (EmuDanger): a beam on the memory screen is its laser while that is on.
    public Collider memoryScreen;      // a trigger over the memory screen's quad
    public EmuDanger danger;

    private const int Nothing = -1, Display = 0, Memory = -2, Console = -3;   // a target; 1 and up: keyboard number + 1

    private float[] memoryU = new float[2];
    private float[] memoryV = new float[2];
    private bool[] burning = new bool[2];   // the trigger is held on the memory screen

    private int[] target = new int[2];
    private int[] key = new int[2];
    private int[] px = new int[2];
    private int[] py = new int[2];
    private bool[] trigger = new bool[2];
    private bool[] grip = new bool[2];
    private int[] pressedOn = new int[2];   // the keyboard a hand holds a key on, or -1
    private bool[] onTube = new bool[2];    // the display it points at is its own classroom tube
    private int owner = 1;                  // the hand the machine's pointer follows
    private float stick;                    // the right stick, up positive
    private float turned;                   // notches of the wheel not sent yet
    private bool started;
    private bool aimShown;                  // an aim's beam is on
    private bool[] landedOn = new bool[2];  // a hand's beam is on something of the room's
    private Collider linksPlate;            // the links panel, in front of the display while open

    private void Init()
    {
        if (started) return;
        started = true;
        for (int h = 0; h < 2; h++)
        {
            target[h] = Nothing;
            key[h] = -1;
            pressedOn[h] = -1;
        }
        if (machine.linksPanel != null) linksPlate = machine.linksPanel.GetComponent<Collider>();
        ShowPitch();
    }

    void Start()
    {
        Init();
    }

    // ---- where a hand points ----

    // Along the back of the hand, from the avatar's bones: wrist to knuckles, or elbow to wrist
    // for a hand without fingers. False when the avatar has neither.
    private bool BoneRay(VRCPlayerApi player, bool leftHand, out Vector3 origin, out Vector3 direction)
    {
        Vector3 wrist = player.GetBonePosition(leftHand ? HumanBodyBones.LeftHand : HumanBodyBones.RightHand);
        Vector3 middle = player.GetBonePosition(leftHand ? HumanBodyBones.LeftMiddleProximal : HumanBodyBones.RightMiddleProximal);
        Vector3 index = player.GetBonePosition(leftHand ? HumanBodyBones.LeftIndexProximal : HumanBodyBones.RightIndexProximal);
        Vector3 little = player.GetBonePosition(leftHand ? HumanBodyBones.LeftLittleProximal : HumanBodyBones.RightLittleProximal);
        origin = wrist;
        direction = Vector3.forward;
        if (wrist == Vector3.zero) return false;
        if (middle != Vector3.zero && (middle - wrist).sqrMagnitude > 1e-6f)
        {
            origin = middle;
            direction = (middle - wrist).normalized;
        }
        else
        {
            Vector3 elbow = player.GetBonePosition(leftHand ? HumanBodyBones.LeftLowerArm : HumanBodyBones.RightLowerArm);
            if (elbow == Vector3.zero || (wrist - elbow).sqrMagnitude < 1e-6f) return false;
            direction = (wrist - elbow).normalized;
        }
        if (pitch != 0f && index != Vector3.zero && little != Vector3.zero)
        {
            // the palm faces along this; turning about the line across the knuckles tips the beam
            Vector3 palm = Vector3.Cross(index - wrist, little - wrist);
            if (!leftHand) palm = -palm;
            Vector3 across = Vector3.Cross(palm, direction);
            if (across.sqrMagnitude > 1e-9f) direction = Quaternion.AngleAxis(pitch, across.normalized) * direction;
        }
        return true;
    }

    // What the ray meets first of the display and the keyboards.
    private void Aim(int h, Vector3 origin, Vector3 direction, bool drawBeam)
    {
        Ray ray = new Ray(origin, direction);
        RaycastHit hit;
        float best = maxDistance;
        int found = Nothing, foundKey = -1;
        bool landed = false, tube = false;
        Vector3 at = origin;
        // a beam that meets the open links panel is VRChat's to use, not the machine's pointer
        bool covered = linksPlate != null && machine.linksPanel.activeSelf && linksPlate.Raycast(ray, out hit, best);
        if (!away && !covered && screen.Raycast(ray, out hit, best))
        {
            landed = true;
            best = hit.distance;
            at = hit.point;
            int w = machine.shownWidth, ht = machine.shownHeight;   // another player's, while theirs is shown
            if (w > 0 && ht > 0)
            {
                Vector3 local = screen.transform.InverseTransformPoint(hit.point);
                // the picture sits centred in the quad, as large as fits
                float shape = (float)w / ht;
                float x = local.x * Mathf.Max(1f, aspect / shape) + 0.5f;
                float y = -local.y * Mathf.Max(1f, shape / aspect) + 0.5f;
                if (x >= 0f && y >= 0f && x < 1f && y < 1f)
                {
                    px[h] = (int)(x * w);
                    py[h] = (int)(y * ht);
                    found = Display;
                }
            }
        }
        if (!away && ownStation >= 0 && ownDisplay && tubes != null && ownStation < tubes.Length && tubes[ownStation].Raycast(ray, out hit, best))
        {
            landed = true;
            best = hit.distance;
            at = hit.point;
            found = Nothing;
            int w = machine.OwnWidth(), ht = machine.OwnHeight();
            if (w > 0 && ht > 0)
            {
                // The plate is flat and the glass behind it is part of a ball: the beam goes on from
                // the plate to the glass, and it is the glass's place that is the picture's.
                Transform plate = tubes[ownStation].transform;
                Vector3 from = hit.point - plate.position;
                float gx = Vector3.Dot(from, plate.right), gy = Vector3.Dot(from, plate.up), gz = 0f;
                float dx = Vector3.Dot(direction, plate.right), dy = Vector3.Dot(direction, plate.up), dz = Vector3.Dot(direction, plate.forward);
                if (dz > 0.05f)
                {
                    for (int step = 0; step < 3; step++)
                    {
                        float glass = tubeGap + tubeBulge - Mathf.Sqrt(Mathf.Max(0f, tubeBulge * tubeBulge - gx * gx - gy * gy));
                        float t = (glass - gz) / dz;
                        gx += dx * t;
                        gy += dy * t;
                        gz += dz * t;
                    }
                    at = plate.position + plate.right * gx + plate.up * gy + plate.forward * gz;
                }
                Vector3 size = plate.lossyScale;
                Vector3 local = new Vector3(gx / size.x, gy / size.y, 0f) / tubeFill;
                float shape = (float)w / ht;
                float x = local.x * Mathf.Max(1f, tubeAspect / shape) + 0.5f;
                float y = -local.y * Mathf.Max(1f, shape / tubeAspect) + 0.5f;
                if (x >= 0f && y >= 0f && x < 1f && y < 1f)
                {
                    px[h] = (int)(x * w);
                    py[h] = (int)(y * ht);
                    found = Display;
                    tube = true;
                }
            }
        }
        if ((!away || holoFlat || holoKeys) && !holoHidden && ownHolo >= 0 && holoScreens != null && ownHolo < holoScreens.Length && holoScreens[ownHolo].Raycast(ray, out hit, best))
        {
            landed = true;
            best = hit.distance;
            at = hit.point;
            found = Nothing;
            int w = machine.OwnWidth(), ht = machine.OwnHeight();
            if (w > 0 && ht > 0)
            {
                // the picture sits centred in the quad, as large as fits, as on the den's wall
                Vector3 local = holoScreens[ownHolo].transform.InverseTransformPoint(hit.point);
                float shape = (float)w / ht;
                float x = local.x * Mathf.Max(1f, holoAspect / shape) + 0.5f;
                float y = -local.y * Mathf.Max(1f, shape / holoAspect) + 0.5f;
                if (x >= 0f && y >= 0f && x < 1f && y < 1f)
                {
                    px[h] = (int)(x * w);
                    py[h] = (int)(y * ht);
                    found = Display;
                    tube = true;   // this visitor's own machine's, as a classroom tube is
                }
            }
        }
        if (!away && danger != null && memoryScreen != null && danger.LaserOn() && memoryScreen.Raycast(ray, out hit, best))
        {
            landed = true;
            best = hit.distance;
            at = hit.point;
            found = Memory;
            // a unit quad: across from its left edge, up from its bottom
            Vector3 local = memoryScreen.transform.InverseTransformPoint(hit.point);
            memoryU[h] = Mathf.Clamp01(local.x + 0.5f);
            memoryV[h] = Mathf.Clamp01(local.y + 0.5f);
        }
        if (holoKeys && ownHolo >= 0 && holoPlates != null && ownHolo < holoPlates.Length && holoPlates[ownHolo].Raycast(ray, out hit, best))
        {
            landed = true;
            best = hit.distance;
            at = hit.point;
            found = Console;
            // a unit plate: across from its left edge, down from its top
            Vector3 local = holoPlates[ownHolo].transform.InverseTransformPoint(hit.point);
            float x = local.x + 0.5f, y = 0.5f - local.y;
            for (int k = 0; k < holoKeyEvents.Length; k++)
            {
                float kx = holoKeyRects[k * 4], ky = holoKeyRects[k * 4 + 1];
                if (x >= kx && y >= ky && x < kx + holoKeyRects[k * 4 + 2] && y < ky + holoKeyRects[k * 4 + 3]) foundKey = k;
            }
        }
        if (holoKeys && ownHolo >= 0 && holoShow != null && ownHolo < holoShow.Length && holoShow[ownHolo] != null
            && holoShow[ownHolo].Raycast(ray, out hit, best))
        {
            landed = true;
            best = hit.distance;
            at = hit.point;
            found = Console;
            foundKey = holoKeyEvents.Length - 1;
        }
        for (int k = 0; k < keyboardPlates.Length; k++)
        {
            bool holo = k == holoKeyboard;
            if (holo ? ownHolo < 0 || holoHidden : away) continue;
            if (!holo && stationKeysFrom >= 0 && k >= stationKeysFrom && k - stationKeysFrom != ownStation) continue;
            if (!keyboardPlates[k].Raycast(ray, out hit, best)) continue;
            landed = true;
            best = hit.distance;
            at = hit.point;
            found = k + 1;
            foundKey = keyboards[k].KeyAt(hit.point);
        }

        // What stands between stops the beam (a classroom's monitor before the wall's display): it
        // used to reach through, and move the pointer from behind a thing the hand pointed at.
        if (landed && Physics.Raycast(ray, out hit, best - 0.03f, 1, QueryTriggerInteraction.Ignore))
        {
            landed = false;
            found = Nothing;
            foundKey = -1;
        }

        // the key under the beam lights up; the one it left goes dark
        int was = target[h];
        if (was >= 1 && was != found) keyboards[was - 1].SetHover(h, -1);
        if (found >= 1) keyboards[found - 1].SetHover(h, foundKey);
        target[h] = found;
        key[h] = foundKey;
        onTube[h] = tube && found == Display;
        if (holoMark != null && (found == Console || was == Console))
        {
            // the console's key under this beam, or under the other hand's
            int hand = found == Console && foundKey >= 0 ? h : target[1 - h] == Console && key[1 - h] >= 0 ? 1 - h : -1;
            // (the small button has no mark: it is not on the console's plate)
            if (hand >= 0 && (hand == h ? foundKey : key[hand]) == holoKeyEvents.Length - 1) hand = -1;
            holoMark.gameObject.SetActive(hand >= 0);
            if (hand >= 0)
            {
                int k = hand == h ? foundKey : key[hand];
                Transform plate = holoPlates[ownHolo].transform;
                holoMark.SetParent(plate, false);
                holoMark.localPosition = new Vector3(holoKeyRects[k * 4] + holoKeyRects[k * 4 + 2] * 0.5f - 0.5f,
                                                     0.5f - holoKeyRects[k * 4 + 1] - holoKeyRects[k * 4 + 3] * 0.5f, -0.003f);
                holoMark.localScale = new Vector3(holoKeyRects[k * 4 + 2], holoKeyRects[k * 4 + 3], 1f);
            }
        }

        // nothing is drawn for a hand that points at neither
        landedOn[h] = landed;
        beams[h].enabled = drawBeam && landed;
        if (drawBeam && landed)
        {
            beams[h].SetPosition(0, origin);
            beams[h].SetPosition(1, at);
        }
        dots[h].gameObject.SetActive(landed);
        if (landed)
        {
            dots[h].position = at;
            Material material = trigger[h] || grip[h] || pressedOn[h] >= 0 ? solidMaterial : faintMaterial;
            beams[h].sharedMaterial = material;
            dotRenderers[h].sharedMaterial = material;
        }
    }

    private void Idle(int h)
    {
        if (target[h] >= 1) keyboards[target[h] - 1].SetHover(h, -1);
        target[h] = Nothing;
        key[h] = -1;
        landedOn[h] = false;
        beams[h].enabled = false;
        dots[h].gameObject.SetActive(false);
    }

    // A hand's aim: on while its program's world is drawn and the hand points at nothing of the room's.
    private void PutAim(int h, Vector3 origin, Vector3 direction, bool on)
    {
        if (aimBeams == null || h >= aimBeams.Length || aimBeams[h] == null) return;
        GameObject beam = aimBeams[h].gameObject;
        if (beam.activeSelf != on) beam.SetActive(on);
        if (!on) return;
        aimBeams[h].SetPositionAndRotation(origin, Quaternion.LookRotation(direction));
        aimShown = true;
    }

    private void NoAims()
    {
        if (!aimShown) return;
        aimShown = false;
        PutAim(0, Vector3.zero, Vector3.forward, false);
        PutAim(1, Vector3.zero, Vector3.forward, false);
    }

    // After the avatar has been posed for this frame, so the beams sit on the hands.
    public override void PostLateUpdate()
    {
        Init();
        VRCPlayerApi player = Networking.LocalPlayer;
        if (player == null) return;
        handsKnown = false;
        if (away && !holoKeys)
        {
            Idle(0);
            Idle(1);
            NoAims();
            return;
        }
        if (player.IsUserInVR())
        {
            for (int h = 0; h < 2; h++)
            {
                Vector3 origin, direction;
                if (!BoneRay(player, h == 0, out origin, out direction))
                {
                    // no arm bones: the tracked hand, whose fingers lie along its x axis
                    VRCPlayerApi.TrackingData hand = player.GetTrackingData(
                        h == 0 ? VRCPlayerApi.TrackingDataType.LeftHand : VRCPlayerApi.TrackingDataType.RightHand);
                    origin = hand.position;
                    direction = hand.rotation * (h == 0 ? Vector3.left : Vector3.right);
                }
                handAt[h] = origin;
                handWay[h] = direction;
                Aim(h, origin, direction, true);
                if (aimHands != 0 || aimShown) PutAim(h, origin, direction, (aimHands >> h & 1) != 0 && !beams[h].enabled);
            }
            if (aimHands == 0) aimShown = false;
            handsKnown = true;
        }
        else
        {
            Idle(0);
            VRCPlayerApi.TrackingData head = player.GetTrackingData(VRCPlayerApi.TrackingDataType.Head);
            Aim(1, head.position, head.rotation * Vector3.forward, false);
            grip[1] = target[1] == Display && Input.GetMouseButton(1);
            if (debugBeam)
            {
                for (int h = 0; h < 2; h++)
                {
                    float side = h == 0 ? -1f : 1f;
                    handAt[h] = head.position + head.rotation * new Vector3(0.22f * side, -0.25f, 0.35f);
                    handWay[h] = head.rotation * Quaternion.Euler(-4f, -9f * side, 0f) * Vector3.forward;
                    PutAim(h, handAt[h], handWay[h], (aimHands >> h & 1) != 0);
                }
                handsKnown = true;
            }
            else NoAims();
        }
        if (danger != null)
        {
            // the laser: the right hand's when both point at the memory screen
            int hand = target[1] == Memory ? 1 : target[0] == Memory ? 0 : -1;
            if (hand < 0) danger.Point(false, 0f, 0f, false);
            else danger.Point(true, memoryU[hand], memoryV[hand], burning[hand]);
        }

        // (the pointer is the controller's, but where a beam is on the room's screen: the only display it reaches then)
        if (away && !holoFlat && target[0] != Display && target[1] != Display) return;
        // The machine has one pointer: a hand holding a button on the display keeps it, then
        // the hand that had it, then whichever points there.
        int other = 1 - owner;
        bool ownerHolds = target[owner] == Display && (trigger[owner] || grip[owner]);
        bool otherHolds = target[other] == Display && (trigger[other] || grip[other]);
        if (!ownerHolds && (otherHolds || (target[owner] != Display && target[other] == Display))) owner = other;
        if (target[owner] == Display)
        {
            machine.SetPointer(px[owner], py[owner], (trigger[owner] ? 1 : 0) | (grip[owner] ? 2 : 0));
            machine.pointerOwn = onTube[owner];
            // the wheel: the mouse's on desktop, the right stick in VR at up to 12 notches a second
            if (player.IsUserInVR()) turned += away ? 0f : stick * 12f * Time.deltaTime;   // (a sitter's stick turns them)
            else turned += Input.GetAxis("Mouse ScrollWheel") * 10f;
            int notches = (int)turned;
            if (notches != 0)
            {
                turned -= notches;
                machine.Wheel(notches);
            }
        }
        else if (!away)   // (with a controller held the pointer and its buttons are still the sticks')
        {
            machine.SetPointerButtons(0);
            turned = 0f;
        }
    }

    public override void InputLookVertical(float value, UdonInputEventArgs args)
    {
        stick = Mathf.Abs(value) < 0.25f ? 0f : value;
    }

    // ---- triggers and grips ----

    private int Hand(UdonInputEventArgs args)
    {
        VRCPlayerApi player = Networking.LocalPlayer;
        if (player == null || !player.IsUserInVR()) return 1;
        return args.handType == HandType.LEFT ? 0 : 1;
    }

    public override void InputUse(bool value, UdonInputEventArgs args)
    {
        Init();
        int h = Hand(args);
        if (value && target[h] == Console)
        {
            if (key[h] >= 0 && holodeck != null) holodeck.SendCustomEvent(holoKeyEvents[key[h]]);
            return;
        }
        if (away && value && !holoFlat && target[h] != Display && target[h] < 1) return;
        if (value)
        {
            if (target[h] == Display)
            {
                trigger[h] = true;
                owner = h;
            }
            else if (target[h] == Memory)
            {
                burning[h] = true;
            }
            else if (target[h] >= 1 && pressedOn[h] < 0)
            {
                pressedOn[h] = target[h] - 1;
                keyboards[pressedOn[h]].Press(h, key[h]);
            }
        }
        else
        {
            trigger[h] = false;
            burning[h] = false;
            if (pressedOn[h] >= 0) keyboards[pressedOn[h]].Release(h);
            pressedOn[h] = -1;
        }
    }

    public override void InputGrab(bool value, UdonInputEventArgs args)
    {
        Init();
        VRCPlayerApi player = Networking.LocalPlayer;
        if (player == null || !player.IsUserInVR()) return;
        int h = Hand(args);
        grip[h] = value && target[h] == Display;
    }

    // A hand's beam is on the holodeck's console: its trigger is that console's.
    public bool OnConsole(int h) { return target[h] == Console; }
    // A hand's beam is on the console or a screen: its trigger and grip are no program's hand's.
    public bool Landed(int h) { return landedOn[h]; }

    // ---- the beam's angle (buttons on the control panel) ----

    public void PitchUp()
    {
        pitch = Mathf.Clamp(pitch - 5f, -60f, 60f);
        ShowPitch();
    }

    public void PitchDown()
    {
        pitch = Mathf.Clamp(pitch + 5f, -60f, 60f);
        ShowPitch();
    }

    private void ShowPitch()
    {
        if (pitchLabel != null) pitchLabel.text = "Beam angle " + (-pitch).ToString("F0");
    }
}
