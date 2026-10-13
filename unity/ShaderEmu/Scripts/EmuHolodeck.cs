using TMPro;
using UdonSharp;
using UnityEngine;
using VRC.SDKBase;
using VRC.Udon.Common;

// The holodecks (docs/holodeck.md): eight rooms, a seat each. Whoever sits in a seat has the
// room. This is local, one a client: it draws this visitor's own program's world round the seat
// they sit in, puts each occupant's display on their room's screen (their own machine's, or
// what EmuStreams receives of another's), and says on each stand what its sitter's controls are.
[UdonBehaviourSyncMode(BehaviourSyncMode.None)]
public class EmuHolodeck : UdonSharpBehaviour
{
    public EmuShareHub hub;
    public EmuStreams streams;
    public EmuMachine machine;
    public EmuPointer pointer;
    public EmuGamepad gamepad;
    public VRCStation[] stations;      // a room's seat
    public Transform[] frames;         // a room's own frame: its door behind, the room towards +z
    public Transform[] eyes;           // where a sitter's eyes are: the program's camera stands there
    public Vector3 roomSize;           // wide, high, deep
    public Transform content;          // the mask and the seal: one room's faces, moved to the room in use
    public Transform scene;            // the program's world (GpuHolodeck.shader)
    public Material sceneMaterial;
    public Transform[] screens;        // a room's screen: a unit quad, scaled
    public Renderer[] screenRenderers;
    public Material[] screenMaterials; // DisplayShow.shader, one a room
    public Vector3 screenAt, screenSize;   // in a room's frame: the small screen, in the console's face
    public Vector3 screenTurn;             // its tip back and its turn about the upright, degrees
    public Vector3 largeAt, largeSize;     // the large screen: upright before the seat
    public Transform[] screenGuards;       // each screen's mark (HoloGuard.shader), which goes where its screen goes
    public TextMeshProUGUI[] doorLabels;   // whose a room is, by its door
    public TextMeshProUGUI[] plateLabels;  // and on its stand: what is held, what a key does
    public TextMeshProUGUI[] scaleLabels;
    public Collider[] plateColliders;      // a console's own, which VRChat's laser presses: off for a sitter in a headset
    // The console can be put away (HideConsole): its model and the mark of it, one for all the
    // rooms, and a room's own plate, keys and screen; a small button in its place brings it back.
    public GameObject consolesModel, consolesGuard;
    public GameObject[] plates, plateGuards, keyPlates, showButtons;
    public Texture2D blackTexture;
    public float plane = 0.35f;        // the world's scale: the plane of the program's view that is 1.28 m wide
    public float mouseCounts = 20f;    // a desktop sitter's mouse: counts told the game for one of Unity's "Mouse X"
    public bool beam = true;           // a sitter in a headset sees where their hand points (the console's Beam key)

    private const int FirstSurface = 13;  // of EmuStreams' surfaces: the holodecks' are 13 to 20

    private int count;
    private int[] occupant;            // a room's sitter, by player number; 0: nobody
    private int[] holds;               // and the controller in their hands
    private Texture[] showing;
    private int own = -1;              // the room this visitor sits in
    private int screenMode;            // this visitor's own screen: 0 the console's small one, 1 the large one before the seat
    private int[] placed;              // a room's screen as it is put now: 0 small, 1 large
    private float nextAt;
    private bool[] trigger = new bool[2], grip = new bool[2];   // left, right: for the machine's host words
    private int[] told = new int[16];
    private int roomMinId, roomMaxId;  // the sitter's room for HoloGuard.shader, which marks only there
    private bool leaving;              // this visitor asked to stand up (a jump): the seat lets them
    private bool respawned;
    private int putOut = -1;           // the room whose seat let go of them unasked
    private bool consoleHidden;        // this sitter put the console away
    private bool flat;                 // their program shows a flat screen now, or draws no world: the large screen, no world
    private float mouseGone;           // a desktop sitter's mouse: how far right it has gone, in counts
    private bool firing;               // and its left button, which is the games' Ctrl
    private const int Ctrl = 29, Pressed = 65536;
    // The programs that are played with both hands, by their numbers in linux/apps/gameid.h:
    // each hand has a beam there. Any other program's aim is the right hand's.
    private int[] twoHanded = { 8 };   // nxphys

    void Start()
    {
        count = stations.Length;
        occupant = new int[count];
        holds = new int[count];
        showing = new Texture[count];
        placed = new int[count];
        content.gameObject.SetActive(false);
        scene.gameObject.SetActive(false);
        sceneMaterial.SetFloat("_Plane", plane);
        roomMinId = VRCShader.PropertyToID("_UdonHoloMin");
        roomMaxId = VRCShader.PropertyToID("_UdonHoloMax");
        VRCShader.SetGlobalVector(roomMinId, Vector4.zero);
        VRCShader.SetGlobalVector(roomMaxId, Vector4.zero);
    }

    // ---- the seats (EmuSeat) ----

    public void Entered(int room, VRCPlayerApi player)
    {
        nextAt = 0f;
        if (!player.isLocal) return;
        own = room;
        EmuShare mine = hub.Mine();
        if (mine != null)
        {
            mine.holo = room;
            hub.StateOut();
        }
        // the world round this seat: the room's faces are where it shows
        Transform frame = frames[room];
        content.SetPositionAndRotation(frame.position, frame.rotation);
        scene.SetPositionAndRotation(eyes[room].position, eyes[room].rotation);
        Vector3 a = frame.TransformPoint(new Vector3(-roomSize.x / 2, 0, 0)), b = frame.TransformPoint(new Vector3(roomSize.x / 2, roomSize.y, roomSize.z));
        sceneMaterial.SetVector("_RoomMin", Vector3.Min(a, b));
        sceneMaterial.SetVector("_RoomMax", Vector3.Max(a, b));
        VRCShader.SetGlobalVector(roomMinId, Vector3.Min(a, b));
        VRCShader.SetGlobalVector(roomMaxId, Vector3.Max(a, b));
        flat = false;
        Arrange();
        if (pointer != null) pointer.ownHolo = room;
        // without a headset the visitor's own keyboard is the game's (Insert gives it back);
        // with one their controllers are, and their beams press the console's keys
        if (!player.IsUserInVR())
        {
            if (gamepad != null && gamepad.keys != null) gamepad.keys.SetCapture(true);
            return;
        }
        if (gamepad != null) gamepad.Take(stations[room]);
        BeamKeys(room, true);
    }

    // A console's keys are the sitter's beams' (VRChat's laser did not reach them from the seat).
    private void BeamKeys(int room, bool on)
    {
        if (pointer != null) pointer.holoKeys = on;
        if (plateColliders != null && room < plateColliders.Length && plateColliders[room] != null) plateColliders[room].enabled = !on;
    }

    public void Exited(int room, VRCPlayerApi player)
    {
        nextAt = 0f;
        if (!player.isLocal) return;
        // A stick stood the sitter up in a headset whatever the station was told: unless they
        // asked (a jump) or were sent back to the start, they are sat down again.
        if (!leaving && own == room)
        {
            putOut = room;
            SendCustomEventDelayedFrames(nameof(SitAgain), 2);
            return;
        }
        Leave();
    }

    public void SitAgain()
    {
        int room = putOut;
        putOut = -1;
        if (room < 0 || own != room) return;
        if (respawned) Leave();
        else stations[room].UseStation(Networking.LocalPlayer);
    }

    public override void OnPlayerRespawn(VRCPlayerApi player)
    {
        if (player == null || !player.isLocal || own < 0) return;
        if (putOut >= 0) respawned = true;   // (SitAgain is on its way: it lets go instead)
        else Leave();
    }

    private void Leave()
    {
        leaving = false;
        respawned = false;
        if (own < 0) return;
        if (gamepad != null)
        {
            gamepad.PutBack();
            if (gamepad.keys != null) gamepad.keys.SetCapture(false);
        }
        BeamKeys(own, false);
        own = -1;
        EmuShare mine = hub.Mine();
        if (mine != null)
        {
            mine.holo = -1;
            hub.StateOut();
        }
        consoleHidden = false;
        flat = false;
        Arrange();
        VRCShader.SetGlobalVector(roomMinId, Vector4.zero);
        VRCShader.SetGlobalVector(roomMaxId, Vector4.zero);
        if (pointer != null) pointer.ownHolo = -1;
    }

    // The seat keeps its sitter whatever the sticks do (they are the game's): jumping stands
    // up a sitter without a headset whose keyboard is their own again (a controller's jump and
    // the space bar are the game's; a headset's way out is both grips, or the console's Exit).
    public override void InputJump(bool value, UdonInputEventArgs args)
    {
        if (!value || own < 0 || gamepad == null || gamepad.Holding() != 0) return;
        if (gamepad.keys != null && gamepad.keys.Capturing()) return;
        leaving = true;
        respawned = false;
        stations[own].ExitStation(Networking.LocalPlayer);
    }

    // End stands the sitter up whatever they hold and whoever has their keyboard: the one way
    // out that needs nothing else to work first (a desktop's, mostly: its keys are the game's).
    public void StandUp()
    {
        if (own < 0) return;
        if (gamepad != null)
        {
            gamepad.PutBack();
            if (gamepad.keys != null) gamepad.keys.SetCapture(false);
        }
        leaving = true;
        respawned = false;
        stations[own].ExitStation(Networking.LocalPlayer);
    }

    public int Own() { return own; }
    public bool Flat() { return own >= 0 && flat; }

    // ---- the console put away and brought back (its Hide key, the small button, the Home key) ----

    public void HideConsole()
    {
        if (own < 0) return;
        consoleHidden = true;
        Arrange();
    }

    public void ShowConsole()
    {
        consoleHidden = false;
        Arrange();
    }

    // ---- where a hand points: a beam in the program's world (EmuPointer draws it, as the machine is told)

    public void ToggleBeam()
    {
        beam = !beam;
        Aims();
    }

    private void Aims()
    {
        if (pointer == null) return;
        VRCPlayerApi player = Networking.LocalPlayer;
        int hands = 0;
        if (beam && own >= 0 && !flat && Utilities.IsValid(player) && (player.IsUserInVR() || pointer.debugBeam))
        {
            hands = 2;
            for (int i = 0; i < twoHanded.Length; i++) if (twoHanded[i] == machine.gameId) hands = 3;
        }
        if (pointer.aimHands != hands) pointer.aimHands = hands;
    }

    private void Put(GameObject[] things, int k, bool on)
    {
        if (things != null && k < things.Length && things[k] != null && things[k].activeSelf != on) things[k].SetActive(on);
    }

    // What of the room this visitor sees: the world or the flat screen, the console or its button.
    private void Arrange()
    {
        bool seated = own >= 0, hidden = seated && consoleHidden, world = seated && !flat;
        if (consolesModel != null) consolesModel.SetActive(!hidden);
        if (consolesGuard != null) consolesGuard.SetActive(!hidden);
        for (int k = 0; k < count; k++)
        {
            bool mine = k == own, away = mine && hidden, screen = !away || flat;
            Put(plates, k, !away);
            Put(plateGuards, k, !away);
            Put(keyPlates, k, !away);
            Put(showButtons, k, away);
            if (screens[k].gameObject.activeSelf != screen) screens[k].gameObject.SetActive(screen);
            if (screenGuards != null && k < screenGuards.Length && screenGuards[k] != null) screenGuards[k].gameObject.SetActive(screen);
        }
        content.gameObject.SetActive(world);
        scene.gameObject.SetActive(world);
        if (pointer != null) pointer.holoFlat = seated && flat;
        Aims();
        nextAt = 0f;
    }

    // ---- a stand's keys: they are the sitter's ----

    public void NextScreen()
    {
        if (own < 0) return;
        screenMode = 1 - screenMode;
        nextAt = 0f;
    }

    public void Larger() { Scale(-0.05f); }
    public void Smaller() { Scale(0.05f); }

    private void Scale(float by)
    {
        if (own < 0) return;
        plane = Mathf.Clamp01(plane + by);
        sceneMaterial.SetFloat("_Plane", plane);
        nextAt = 0f;
    }

    // ---- who sits where, and what each room shows ----

    void Update()
    {
        if (own >= 0 && Input.GetKeyDown(KeyCode.End)) StandUp();
        if (own >= 0 && Input.GetKeyDown(KeyCode.Home)) { consoleHidden = !consoleHidden; Arrange(); }
        // a menu, a level loading, or a program that draws no world: its picture on the large screen
        bool flatNow = own >= 0 && (machine.flatFrame || !machine.worldFrame);
        if (flatNow != flat) { flat = flatNow; Arrange(); }
        Mouse();
        Tell();
        if (Time.time < nextAt) return;
        nextAt = Time.time + 0.25f;
        Aims();   // (the program in front may have changed)
        hub.Prune();
        EmuShare mine = hub.Mine();
        if (mine == null) return;
        int myId = hub.MyId(), others = hub.ShareCount();
        // a room is whoever names it's: this visitor while they sit there, else the first of the others
        for (int k = 0; k < count; k++) { occupant[k] = 0; holds[k] = 0; }
        if (own >= 0) { occupant[own] = myId; holds[own] = mine.holding; }
        for (int i = 0; i < others; i++)
        {
            EmuShare share = hub.ShareAt(i);
            int k = share.holo;
            if (k < 0 || k >= count || k == own) continue;
            if (occupant[k] == 0 || share.ownerId < occupant[k]) { occupant[k] = share.ownerId; holds[k] = share.holding; }
        }
        for (int k = 0; k < count; k++)
        {
            bool me = occupant[k] == myId, theirs = occupant[k] > 0 && !me;
            streams.SetWant(FirstSurface + k, theirs ? occupant[k] : 0);
            int slot = theirs ? streams.SlotOf(occupant[k]) : -1;
            Texture shown = blackTexture;
            int w = 0, h = 0;
            if (me) { shown = machine.displayTexture; w = machine.OwnWidth(); h = machine.OwnHeight(); }
            else if (slot >= 0) { shown = streams.Picture(slot); w = streams.Width(slot); h = streams.Height(slot); }
            Material material = screenMaterials[k];
            if (shown != showing[k])
            {
                showing[k] = shown;
                material.SetTexture("_MainTex", shown);
                material.SetVector("_TexSize", new Vector4(shown.width, shown.height, 0, 0));
            }
            material.SetVector("_Size", new Vector4(w, h, 0, 0));
            // this visitor's own screen is the console's small one or a large one before the seat,
            // as its key says; anybody else's is the console's. Its mark goes with it.
            int large = me && (screenMode == 1 || flat) ? 1 : 0;
            if (placed[k] != large)
            {
                placed[k] = large;
                Transform screen = screens[k];
                screen.localPosition = large == 1 ? largeAt : screenAt;
                screen.localEulerAngles = large == 1 ? Vector3.zero : screenTurn;
                screen.localScale = large == 1 ? largeSize : screenSize;
                if (screenGuards != null && k < screenGuards.Length && screenGuards[k] != null)
                {
                    screenGuards[k].SetPositionAndRotation(screen.position, screen.rotation);
                    screenGuards[k].localScale = screen.lossyScale;
                }
            }

            string who = occupant[k] == 0 ? "FREE" : me ? Networking.LocalPlayer.displayName : NameOf(occupant[k]);
            if (doorLabels[k] != null && doorLabels[k].text != who) doorLabels[k].text = who;
            if (plateLabels[k] != null)
            {
                string says = occupant[k] == 0 ? "Sit down to play" : !me ? who + " plays here"
                            : holds[k] != 0 ? gamepad.SchemeName() + ". Both grips: stand up"
                            : gamepad != null && gamepad.keys != null && gamepad.keys.Capturing() ? "Your keys play. End: stand up"
                            : "Space or End: stand up. Sit again for the keys";
                if (plateLabels[k].text != says) plateLabels[k].text = says;
            }
            if (scaleLabels[k] != null && me) scaleLabels[k].text = "World " + Mathf.RoundToInt((1f - plane) * 100f);
        }
    }

    // A sitter without a headset plays as at a desk (docs/holodeck.md, "Played from the seat"):
    // while their keys are the game's, their mouse turns it and its left button is Ctrl.
    private void Mouse()
    {
        VRCPlayerApi player = Networking.LocalPlayer;
        bool playing = own >= 0 && Utilities.IsValid(player) && !player.IsUserInVR()
                    && gamepad != null && gamepad.keys != null && gamepad.keys.Capturing();
        if (playing) mouseGone = Mathf.Repeat(mouseGone + Input.GetAxis("Mouse X") * mouseCounts, 65536f);
        bool fire = playing && Input.GetMouseButton(0);
        if (fire == firing) return;
        firing = fire;
        if (gamepad == null || gamepad.keys == null) return;
        if (machine.inputAway) machine.RemoteKey(fire ? Ctrl | Pressed : Ctrl);
        else gamepad.keys.PushEvent(fire ? Ctrl | Pressed : Ctrl);
    }

    // The program's world goes round with a desktop sitter's head: what turns the game is their
    // mouse, which turns their head too, and the game would turn twice as far as they looked.
    public override void PostLateUpdate()
    {
        VRCPlayerApi player = Networking.LocalPlayer;
        if (own < 0 || !Utilities.IsValid(player) || player.IsUserInVR()) return;
        VRCPlayerApi.TrackingData head = player.GetTrackingData(VRCPlayerApi.TrackingDataType.Head);
        scene.SetPositionAndRotation(head.position, Quaternion.Euler(0f, head.rotation.eulerAngles.y, 0f));
    }

    // ---- what the machine is told of where it is shown (docs/holodeck.md, "What the machine is told")

    public override void InputUse(bool value, UdonInputEventArgs args) { trigger[args.handType == HandType.LEFT ? 0 : 1] = value; }
    public override void InputGrab(bool value, UdonInputEventArgs args) { grip[args.handType == HandType.LEFT ? 0 : 1] = value; }

    // A sixteenth of a word: a length in millimetres, or a turn in 65,536ths, two's complement.
    private int Half(float v) { return Mathf.Clamp(Mathf.RoundToInt(v), -32768, 32767) & 0xffff; }
    private int Turn(float degrees) { return Mathf.RoundToInt(Mathf.DeltaAngle(0f, degrees) * (65536f / 360f)) & 0xffff; }

    // Eight words in halves: against the seat's eye point while seated, else the visitor's own feet.
    private void Tell()
    {
        VRCPlayerApi player = Networking.LocalPlayer;
        if (!Utilities.IsValid(player) || machine.machineMaterial == null) return;
        Vector3 origin = own >= 0 ? eyes[own].position : player.GetPosition();
        Quaternion back = Quaternion.Inverse(own >= 0 ? eyes[own].rotation : player.GetRotation());
        VRCPlayerApi.TrackingData head = player.GetTrackingData(VRCPlayerApi.TrackingDataType.Head);
        Vector3 at = back * (head.position - origin) * 1000f, turn = (back * head.rotation).eulerAngles;
        int[] now = new int[16];
        // (a test's invented hands are told as a headset's)
        bool headset = player.IsUserInVR() || (pointer != null && pointer.debugBeam);
        // (bit 5: the hands are tracked, and a program may aim by one)
        now[0] = (headset ? 3 | 32 : 2) | (own >= 0 ? 16 : 0) | (Mathf.Min(VRCPlayerApi.GetPlayerCount(), 255) << 8);
        now[1] = (player.playerId & 255) | (Mathf.RoundToInt(plane * 255f) << 8);   // (and the room's scale)
        now[2] = Turn(turn.y);    // to the right
        now[3] = Turn(-turn.x);   // upwards
        now[4] = Half(at.x);
        now[5] = Half(at.y);
        now[6] = Half(at.z);
        now[7] = Turn(turn.z);
        for (int h = 0; h < 2; h++)
        {
            VRCPlayerApi.TrackingData hand = player.GetTrackingData(h == 0 ? VRCPlayerApi.TrackingDataType.LeftHand : VRCPlayerApi.TrackingDataType.RightHand);
            Vector3 from = hand.position, along = hand.rotation * (h == 0 ? Vector3.left : Vector3.right);
            // (a game aims and holds its weapon where the visitor's own beam is: EmuPointer's)
            if (pointer != null && pointer.handsKnown) { from = pointer.handAt[h]; along = pointer.handWay[h]; }
            Vector3 place = back * (from - origin) * 250f, way = back * along;
            // its place in 4 mm, ten bits each way, with its trigger and grip; then where it points:
            // its turn to the right and its rise, 65,536 to a turn as the head's
            int x = Mathf.Clamp(Mathf.RoundToInt(place.x), -512, 511) & 1023, y = Mathf.Clamp(Mathf.RoundToInt(place.y), -512, 511) & 1023;
            int z = Mathf.Clamp(Mathf.RoundToInt(place.z), -512, 511) & 1023;
            int word = x | (y << 10) | (z << 20);
            // (a hand whose beam is on the console or a screen presses that: no program's hand takes by it)
            bool free = pointer == null || !pointer.Landed(h);
            now[8 + h * 4] = word & 0xffff;
            now[9 + h * 4] = ((word >> 16) & 0x3fff) | (trigger[h] && free ? 1 << 14 : 0) | (grip[h] && free ? 1 << 15 : 0);
            now[10 + h * 4] = Turn(Mathf.Atan2(way.x, way.z) * Mathf.Rad2Deg);
            now[11 + h * 4] = Turn(Mathf.Asin(Mathf.Clamp(way.y, -1f, 1f)) * Mathf.Rad2Deg);
        }
        // (no hands: the first of their words is how far a sitter's mouse has gone to the right;
        // with hands, the left one's second word is the sticks)
        if (!headset) now[8] = Mathf.RoundToInt(mouseGone) & 0xffff;
        else if (gamepad != null) { now[10] = gamepad.StickLow(); now[11] = gamepad.StickHigh(); }
        for (int i = 0; i < 16; i++)
        {
            if (now[i] == told[i]) continue;
            told[i] = now[i];
            machine.machineMaterial.SetInt("_HostS" + i, now[i]);
        }
    }

    private string NameOf(int id)
    {
        int others = hub.ShareCount();
        for (int i = 0; i < others; i++)
        {
            EmuShare share = hub.ShareAt(i);
            if (share.ownerId == id) return share.ownerName;
        }
        return "?";
    }
}
