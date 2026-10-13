using System.Collections.Generic;
using TMPro;
using UdonSharp;
using UdonSharpEditor;
using UnityEditor;
using UnityEngine;
using UnityEngine.Rendering;
using UnityEngine.UI;
using VRC.Udon;

// Beyond the den's two doors (world/annex.py, docs/holodeck.md): the holodecks, eight rooms off a
// corridor where a 3D program's frame is a world round whoever sits there, and the corridor a
// visitor arrives in. The models are the annex's and the shell's; this gives them their
// colliders, lights, seats, screens and controls.
public static partial class ShaderEmuBuilder
{
    // as world/room.py and world/annex.py
    const float HoloZ = 3.9f, HoloWide = 1.3f, HoloHigh = 2.3f;
    const float CorX0 = -6f, CorX1 = 4.5f, CorWide = 2.4f, CorHigh = 2.8f;
    const float BackDoorX = 3.4f, BackDoorWide = 0.95f, BackDoorHigh = 2.1f;

    // Models/holodeck.json, which world/annex.py writes with the models
    [System.Serializable]
    class HolodeckPlan
    {
        public float[] rooms;      // x, z and turn of each room's frame, one after another
        public float[] room;       // a room's side, its height
        public float[] door;       // its doorway: wide, high
        public float[] corridor;   // x0, x1, z0, z1, height
        public float seat;         // the seat, from the door
        public float[] screen;     // its middle (x, y, z), wide, high, in a room's frame
        public float[] screenTurn; // its tip back and its turn about the upright, degrees
        public float[] panel;      // the keys' middle on the console's face
        public float[] stand;      // the console's foot: x, z
        public float[] lamps;      // the corridor's, along x
    }

    static void Solid(Transform parent, string name, float x0, float x1, float y0, float y1, float z0, float z1)
    {
        GameObject go = new GameObject(name);
        go.transform.SetParent(parent, false);
        go.transform.localPosition = new Vector3((x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2);
        go.AddComponent<BoxCollider>().size = new Vector3(Mathf.Abs(x1 - x0), Mathf.Abs(y1 - y0), Mathf.Abs(z1 - z0));
        go.isStatic = true;
    }

    static void Gone(Transform parent, string name)
    {
        for (int i = parent.childCount - 1; i >= 0; i--)
            if (parent.GetChild(i).name == name) Object.DestroyImmediate(parent.GetChild(i).gameObject);
    }

    // A wall along x from x0 to x1 at z, `thick` towards +z, with gaps for doorways (pairs of x).
    static void WallX(Transform parent, string name, float x0, float x1, float z, float thick, float high, float doorHigh, List<float> gaps)
    {
        float from = x0;
        for (int i = 0; i + 1 < gaps.Count; i += 2)
        {
            Solid(parent, name, from, gaps[i], 0, high, z, z + thick);
            Solid(parent, name, gaps[i], gaps[i + 1], doorHigh, high, z, z + thick);
            from = gaps[i + 1];
        }
        Solid(parent, name, from, x1, 0, high, z, z + thick);
    }

    // A holodeck's own things stay before the program's world (HoloGuard.shader): a copy of the
    // seats', stands' and screens' models, of each screen's glass and of each stand's panel,
    // which draws only a mark where the thing itself is seen.
    const float SeatHeight = 0.23f;   // where a sitter's hips are put (0.50 and 0.38 sat them too high in a headset)

    static void HoloGuards(Transform world)
    {
        Transform holodeck = world.Find("Holodeck"), models = world.Find("Models");
        if (holodeck == null || models == null) throw new System.Exception("no holodecks in the open scene");
        Gone(holodeck, "Holodeck guards");
        Transform guards = new GameObject("Holodeck guards").transform;
        guards.SetParent(holodeck, false);
        Material mark = Mat("HoloGuard", "ShaderEmu/HoloGuard");
        List<MeshFilter> things = new List<MeshFilter>();
        foreach (MeshFilter filter in models.GetComponentsInChildren<MeshFilter>(true))
            if (filter.name == "Holo seats" || filter.name == "Holo consoles") things.Add(filter);
        Mesh quad = null;
        foreach (EmuHolodeck deck in holodeck.GetComponentsInChildren<EmuHolodeck>(true))
        {
            UdonSharpEditorUtility.CopyUdonToProxy(deck);
            foreach (Transform screen in deck.screens)
            {
                things.Add(screen.GetComponent<MeshFilter>());
                quad = screen.GetComponent<MeshFilter>().sharedMesh;
            }
        }
        Dictionary<Transform, Transform> copies = new Dictionary<Transform, Transform>();
        foreach (MeshFilter thing in things)
        {
            GameObject copy = new GameObject(thing.name + " guard", typeof(MeshFilter), typeof(MeshRenderer));
            copy.transform.SetParent(guards, false);
            copy.transform.SetPositionAndRotation(thing.transform.position, thing.transform.rotation);
            copy.transform.localScale = thing.transform.lossyScale;
            copy.GetComponent<MeshFilter>().sharedMesh = thing.sharedMesh;
            copies[thing.transform] = copy.transform;
        }
        // a screen's mark goes where the sitter puts the screen (EmuHolodeck.NextScreen)
        foreach (EmuHolodeck deck in holodeck.GetComponentsInChildren<EmuHolodeck>(true))
        {
            deck.screenGuards = new Transform[deck.screens.Length];
            for (int i = 0; i < deck.screens.Length; i++) deck.screenGuards[i] = copies[deck.screens[i]];
            Apply(deck);
        }
        // a stand's panel is a canvas a little over its desk: a rectangle of its size
        foreach (Canvas canvas in holodeck.GetComponentsInChildren<Canvas>(true))
        {
            if (!canvas.name.EndsWith(" plate") || quad == null) continue;
            RectTransform rect = (RectTransform)canvas.transform;
            GameObject copy = new GameObject(canvas.name + " guard", typeof(MeshFilter), typeof(MeshRenderer));
            copy.transform.SetParent(guards, false);
            copy.transform.SetPositionAndRotation(rect.position, rect.rotation);
            copy.transform.localScale = new Vector3(rect.rect.width * rect.lossyScale.x, rect.rect.height * rect.lossyScale.y, 1f);
            copy.GetComponent<MeshFilter>().sharedMesh = quad;
        }
        foreach (MeshRenderer renderer in guards.GetComponentsInChildren<MeshRenderer>(true))
        {
            Material[] all = new Material[Mathf.Max(1, renderer.GetComponent<MeshFilter>().sharedMesh.subMeshCount)];
            for (int i = 0; i < all.Length; i++) all[i] = mark;
            renderer.sharedMaterials = all;
            renderer.shadowCastingMode = ShadowCastingMode.Off;
            renderer.receiveShadows = false;
            renderer.lightProbeUsage = LightProbeUsage.Off;
            renderer.reflectionProbeUsage = ReflectionProbeUsage.Off;
        }
    }

    // The large screen a sitter may have in place of the console's small one: upright and
    // straight before the seat, 1.7 m off, its middle a little over a sitter's eyes (1.08 m),
    // of the small one's shape. The console, to the right and nearer, covers none of it from there.
    static void LargeScreen(EmuHolodeck deck, float aspect, float side)
    {
        const float wide = 1.6f;
        deck.largeAt = new Vector3(0f, 1.25f, side - 1.0f);
        deck.largeSize = new Vector3(wide, wide / aspect, 1f);
    }

    [MenuItem("ShaderEmu/Add the holodecks' large screens to the open scene")]
    public static void AddLargeScreens()
    {
        Transform world = GameObject.Find("ShaderEmu").transform;
        foreach (EmuHolodeck deck in world.GetComponentsInChildren<EmuHolodeck>(true))
        {
            UdonSharpEditorUtility.CopyUdonToProxy(deck);
            Transform small = deck.screens[0];
            deck.screenTurn = small.localEulerAngles;
            LargeScreen(deck, deck.screenSize.x / deck.screenSize.y, deck.roomSize.x);
            Apply(deck);
        }
        HoloGuards(world);
        UnityEditor.SceneManagement.EditorSceneManager.MarkSceneDirty(world.gameObject.scene);
        AssetDatabase.SaveAssets();
        Debug.Log("[ShaderEmu] the holodecks have their large screens (the scene is not saved)");
    }

    // The console's keys: x, y, width, height on its face of ConsoleWide by ConsoleHigh, and the
    // event of EmuHolodeck each sends. A sitter's controls are picked by the game (EmuGamepad).
    const float ConsoleWide = 380, ConsoleHigh = 280;
    static readonly float[] ConsoleKeys = { 12, 72, 174, 92, 194, 72, 174, 92, 12, 174, 112, 94, 132, 174, 112, 94 };
    static readonly string[] ConsoleEvents = { "NextScreen", "StandUp", "Larger", "Smaller" };
    static readonly string[] ConsoleNames = { "Screen", "Exit", "World +", "World -" };

    // The console's face: the room's keys, in the screen's plane under it. Exit stands the sitter
    // up whatever they hold (EmuHolodeck.StandUp): the one key for it, a headset's or a desktop's.
    static void ConsolePanel(Transform frame, int k, Vector3 at, Vector3 turn, EmuHolodeck deck, List<KeyValuePair<Button, string>> keys)
    {
        Color back = new Color(0.05f, 0.055f, 0.07f), dim = new Color(0.62f, 0.68f, 0.76f);
        TextMeshProUGUI unused;
        RectTransform panel = Panel(frame, "Holodeck " + (k + 1) + " plate", at, turn, ConsoleWide, ConsoleHigh, back);
        Label(panel, "Title", "HOLODECK " + (k + 1), 12, 6, 356, 32, 24, TextAnchor.MiddleLeft, Color.white);
        deck.plateLabels[k] = Label(panel, "Says", "Sit down to play", 12, 38, 356, 28, 18, TextAnchor.MiddleLeft, new Color(1f, 0.8f, 0.3f));
        for (int i = 0; i < ConsoleEvents.Length; i++)
            keys.Add(new KeyValuePair<Button, string>(MakeButton(panel, ConsoleEvents[i], ConsoleNames[i],
                     ConsoleKeys[i * 4], ConsoleKeys[i * 4 + 1], ConsoleKeys[i * 4 + 2], ConsoleKeys[i * 4 + 3], 26, out unused), ConsoleEvents[i]));
        deck.scaleLabels[k] = Label(panel, "Scale", "World 65", 252, 174, 116, 94, 20, TextAnchor.MiddleCenter, dim);
    }

    // The consoles' keys to a sitter's own beams (EmuPointer): a plate a room a hair before its
    // face, and a mark for the key a beam is on. VRChat's laser did not press them from the seat.
    static void ConsoleBeams(Transform world, EmuHolodeck deck)
    {
        int rooms = deck.frames.Length;
        Collider[] plates = new Collider[rooms];
        deck.plateColliders = new Collider[rooms];
        for (int k = 0; k < rooms; k++)
        {
            Transform frame = deck.frames[k], face = frame.Find("Holodeck " + (k + 1) + " plate");
            Gone(frame, "Keys plate");
            if (face == null) continue;
            deck.plateColliders[k] = face.GetComponent<BoxCollider>();
            GameObject plate = new GameObject("Keys plate");
            plate.transform.SetParent(frame, false);
            plate.transform.localRotation = face.localRotation;
            plate.transform.localPosition = face.localPosition + face.localRotation * new Vector3(0, 0, -0.002f);
            plate.transform.localScale = new Vector3(ConsoleWide * 0.001f, ConsoleHigh * 0.001f, 1f);
            BoxCollider box = plate.AddComponent<BoxCollider>();
            box.size = new Vector3(1f, 1f, 0.004f);
            box.isTrigger = true;
            plates[k] = box;
        }
        Gone(deck.transform, "Key mark");
        GameObject mark = GameObject.CreatePrimitive(PrimitiveType.Quad);
        mark.name = "Key mark";
        Object.DestroyImmediate(mark.GetComponent<Collider>());
        mark.transform.SetParent(deck.transform, false);
        Material over = Mat("Holo key over", "Sprites/Default");
        over.color = new Color(1f, 0.75f, 0.2f, 0.35f);
        MeshRenderer markRenderer = mark.GetComponent<MeshRenderer>();
        markRenderer.sharedMaterial = over;
        markRenderer.shadowCastingMode = ShadowCastingMode.Off;
        markRenderer.receiveShadows = false;
        mark.SetActive(false);
        float[] rects = new float[ConsoleKeys.Length];
        for (int i = 0; i < rects.Length; i++) rects[i] = ConsoleKeys[i] / (i % 2 == 0 ? ConsoleWide : ConsoleHigh);
        foreach (EmuPointer pointer in world.GetComponentsInChildren<EmuPointer>(true))
        {
            UdonSharpEditorUtility.CopyUdonToProxy(pointer);
            pointer.holoPlates = plates;
            pointer.holoKeyRects = rects;
            pointer.holoKeyEvents = ConsoleEvents;
            pointer.holodeck = deck;
            pointer.holoMark = mark.transform;
            Apply(pointer);
        }
        foreach (EmuGamepad gamepad in world.GetComponentsInChildren<EmuGamepad>(true))
        {
            UdonSharpEditorUtility.CopyUdonToProxy(gamepad);
            gamepad.holodeck = deck;
            Apply(gamepad);
        }
    }

    // The consoles' faces made again where they are, in a scene from before their keys were the
    // beams' and the controls the game's: new plates, which take the probes' light until the next bake.
    [MenuItem("ShaderEmu/Make the holodecks' consoles again in the open scene")]
    public static void ConsolesAgain()
    {
        Transform world = GameObject.Find("ShaderEmu").transform;
        int count = 0;
        foreach (EmuHolodeck deck in world.GetComponentsInChildren<EmuHolodeck>(true))
        {
            UdonSharpEditorUtility.CopyUdonToProxy(deck);
            List<KeyValuePair<Button, string>> keys = new List<KeyValuePair<Button, string>>();
            for (int k = 0; k < deck.frames.Length; k++)
            {
                Transform frame = deck.frames[k], old = frame.Find("Holodeck " + (k + 1) + " plate");
                if (old == null) continue;
                Vector3 at = old.localPosition, turn = old.localEulerAngles;
                Object.DestroyImmediate(old.gameObject);
                ConsolePanel(frame, k, at, turn, deck, keys);
                count++;
            }
            ConsoleBeams(world, deck);
            Apply(deck);
            foreach (KeyValuePair<Button, string> key in keys) OnClick(key.Key, deck, key.Value);
        }
        RetroNewPanels();
        HoloGuards(world);
        UnityEditor.SceneManagement.EditorSceneManager.MarkSceneDirty(world.gameObject.scene);
        Debug.Log("[ShaderEmu] " + count + " consoles are made again (the scene is not saved; their plates want a bake)");
    }

    // A scene from before the seats waited on the floor (EmuSeat): each put there, its reach left where it was.
    [MenuItem("ShaderEmu/Make the holodecks' seats rise under their sitters in the open scene")]
    public static void SeatsRise()
    {
        Transform world = GameObject.Find("ShaderEmu").transform;
        int count = 0;
        foreach (EmuSeat sits in world.GetComponentsInChildren<EmuSeat>(true))
        {
            UdonSharpEditorUtility.CopyUdonToProxy(sits);
            sits.height = SeatHeight;
            sits.settle = 0.5f;
            Apply(sits);
            Transform seat = sits.transform;
            seat.localPosition = new Vector3(seat.localPosition.x, 0, seat.localPosition.z);
            BoxCollider reach = seat.GetComponent<BoxCollider>();
            if (reach != null) reach.center = new Vector3(0, 0.15f + SeatHeight, 0);
            // (the program's camera stands at the sitter's eyes: it comes with them)
            Transform eye = seat.parent.Find("Seat eye");
            if (eye != null) eye.localPosition = new Vector3(eye.localPosition.x, SeatHeight + 0.7f, eye.localPosition.z);
            EditorUtility.SetDirty(seat);
            count++;
        }
        UnityEditor.SceneManagement.EditorSceneManager.MarkSceneDirty(world.gameObject.scene);
        Debug.Log("[ShaderEmu] " + count + " seats wait on the floor and rise under their sitters (the scene is not saved)");
    }

    // A program's textures, light maps and models may be files in the ROM (Quake III Arena's all are).
    static void GiveRom(Material m)
    {
        for (int lane = 0; lane < 4; lane++)
            m.SetTexture("_Data_MTD_" + "RGBA"[lane], AssetDatabase.LoadAssetAtPath<Texture2D>(ShaderEmuImages.LanePath(ShaderEmuImages.Images[0].name, "rom", lane)));
        EditorUtility.SetDirty(m);
    }

    [MenuItem("ShaderEmu/Give the holodecks' world the ROM")]
    public static void GiveHolodecksRom()
    {
        foreach (string name in new[] { "GpuHolodeck", "GpuVolume" })
        {
            Material m = AssetDatabase.LoadAssetAtPath<Material>(Generated + "/" + name + ".mat");
            if (m != null) GiveRom(m);
        }
        AssetDatabase.SaveAssets();
        Debug.Log("[ShaderEmu] the holodecks' world reads the ROM");
    }

    [MenuItem("ShaderEmu/Add the holodecks' guards to the open scene")]
    public static void AddHoloGuards()
    {
        Transform world = GameObject.Find("ShaderEmu").transform;
        HoloGuards(world);
        foreach (string name in new[] { "GpuHolodeck", "VolumeSeal" })
        {
            Material m = AssetDatabase.LoadAssetAtPath<Material>(Generated + "/" + name + ".mat");
            if (m == null) continue;
            m.renderQueue = name == "GpuHolodeck" ? 2503 : 2504;
            EditorUtility.SetDirty(m);
        }
        UnityEditor.SceneManagement.EditorSceneManager.MarkSceneDirty(world.gameObject.scene);
        UnityEditor.SceneManagement.EditorSceneManager.SaveOpenScenes();
        AssetDatabase.SaveAssets();
        Debug.Log("[ShaderEmu] the holodecks' own things are guarded");
    }

    static void Annex(Transform world, Transform models, Dictionary<string, Material> materials)
    {
        const float halfW = 4.5f, halfD = 5.5f, height = 3.2f, wall = 0.2f;
        HolodeckPlan plan = JsonUtility.FromJson<HolodeckPlan>(System.IO.File.ReadAllText(ModelsPath + "/holodeck.json"));
        int rooms = plan.rooms.Length / 3;
        float side = plan.room[0], tall = plan.room[1], doorWide = plan.door[0], doorHigh = plan.door[1];
        float hx0 = plan.corridor[0], hx1 = plan.corridor[1], hz0 = plan.corridor[2], hz1 = plan.corridor[3], hHigh = plan.corridor[4];
        Transform room = world.Find("Room"), decor = world.Find("Decor"), computer = world.Find("Computer");
        float corZ1 = -halfD - wall, corZ0 = corZ1 - CorWide;

        // ---- what stops a visitor: the den's two walls with their doorways, and the new rooms' own
        Gone(room, "Wall left");
        Gone(room, "Wall back");
        foreach (string door in new[] { "Door", "Door frame top", "Door frame side", "Door knob" }) Gone(decor, door);
        Gone(world, "Annex colliders");
        Transform solid = new GameObject("Annex colliders").transform;
        solid.SetParent(world, false);
        float dz0 = HoloZ - HoloWide / 2, dz1 = HoloZ + HoloWide / 2, dx0 = BackDoorX - BackDoorWide / 2, dx1 = BackDoorX + BackDoorWide / 2;
        Solid(solid, "Wall left", -halfW - wall, -halfW, 0, height, -halfD, dz0);
        Solid(solid, "Wall left", -halfW - wall, -halfW, 0, height, dz1, halfD);
        Solid(solid, "Wall left", -halfW - wall, -halfW, HoloHigh, height, dz0, dz1);
        Solid(solid, "Wall back", CorX0, dx0, 0, height, -halfD - wall, -halfD);
        Solid(solid, "Wall back", dx1, CorX1 + wall, 0, height, -halfD - wall, -halfD);
        Solid(solid, "Wall back", dx0, dx1, BackDoorHigh, height, -halfD - wall, -halfD);
        Solid(solid, "Corridor floor", CorX0, CorX1, -0.1f, 0, corZ0, -halfD);
        Solid(solid, "Corridor ceiling", CorX0, CorX1, CorHigh, CorHigh + 0.1f, corZ0, corZ1);
        Solid(solid, "Corridor wall", CorX0, CorX1, 0, CorHigh, corZ0 - wall, corZ0);
        Solid(solid, "Corridor wall", CorX0 - wall, CorX0, 0, CorHigh, corZ0, corZ1);
        Solid(solid, "Corridor wall", CorX1, CorX1 + wall, 0, CorHigh, corZ0, corZ1);
        Solid(solid, "Corridor bench", -2.55f, -1.35f, 0, 0.45f, corZ1 - 0.5f, corZ1);
        // the holodecks' corridor, and a box of walls round each room with its doorway left open
        Solid(solid, "Holo corridor floor", hx0, -halfW, -0.1f, 0, hz0, hz1);
        Solid(solid, "Holo corridor ceiling", hx0, hx1, hHigh, hHigh + 0.1f, hz0, hz1);
        Solid(solid, "Holo corridor wall", hx0 - wall, hx0, 0, hHigh, hz0, hz1);
        List<float> north = new List<float>(), south = new List<float>();
        for (int k = rooms - 1; k >= 0; k--)   // (rooms run towards -x: the gaps in rising x)
            (k % 2 == 0 ? north : south).AddRange(new[] { plan.rooms[k * 3] - doorWide / 2, plan.rooms[k * 3] + doorWide / 2 });
        WallX(solid, "Holo corridor wall", hx0, hx1, hz1, wall, tall, doorHigh, north);
        WallX(solid, "Holo corridor wall", hx0, hx1, hz0 - wall, wall, tall, doorHigh, south);

        // ---- a visitor steps out of the lift
        foreach (GameObject go in world.gameObject.scene.GetRootGameObjects())
            if (go.name == "VRCWorld") go.transform.SetPositionAndRotation(new Vector3(CorX0 + 1.1f, 0, (corZ0 + corZ1) / 2), Quaternion.Euler(0, 90, 0));

        // ---- light: the corridors' lamps, and a little in each room besides its grid's own
        foreach (string old in new[] { "Corridor light", "Holodeck light", "Holo corridor light", "Reflection probe corridor", "Reflection probe holodeck",
                                       "Reflection probe holo corridor" })
            Gone(decor, old);
        foreach (float x in new[] { -3.9f, -0.4f, 3.1f })
            BakedLight(decor, "Corridor light", new Vector3(x, CorHigh - 0.3f, (corZ0 + corZ1) / 2), new Color(1f, 0.84f, 0.62f), 1.3f, 5f);
        foreach (float x in plan.lamps)
            BakedLight(decor, "Holo corridor light", new Vector3(x, hHigh - 0.25f, HoloZ), new Color(1f, 0.86f, 0.66f), 1.1f, 5f);
        foreach (string old in new[] { "Light volume corridor", "Light volume holodeck", "Light volume holo corridor", "Light volume holodecks north",
                                       "Light volume holodecks south" })
            Gone(world, old);
        LightVolumeIn(world, "Light volume corridor", new Vector3((CorX0 + CorX1) / 2, CorHigh / 2, (corZ0 + corZ1) / 2), new Vector3(CorX1 - CorX0, CorHigh, CorWide));
        LightVolumeIn(world, "Light volume holo corridor", new Vector3((hx0 + hx1) / 2, hHigh / 2, HoloZ), new Vector3(hx1 - hx0, hHigh, hz1 - hz0));
        LightVolumeIn(world, "Light volume holodecks north", new Vector3((hx0 + hx1) / 2, tall / 2, hz1 + wall + side / 2), new Vector3(hx1 - hx0, tall, side));
        LightVolumeIn(world, "Light volume holodecks south", new Vector3((hx0 + hx1) / 2, tall / 2, hz0 - wall - side / 2), new Vector3(hx1 - hx0, tall, side));
        ProbeIn(decor, "Reflection probe corridor", new Vector3((CorX0 + CorX1) / 2, CorHigh / 2, (corZ0 + corZ1) / 2), new Vector3(CorX1 - CorX0, CorHigh, CorWide));
        ProbeIn(decor, "Reflection probe holo corridor", new Vector3((hx0 + hx1) / 2, hHigh / 2, HoloZ), new Vector3(hx1 - hx0, hHigh, hz1 - hz0));

        // ---- the holodecks: the volume display's renderers, with the faces of the room in use as the window
        Gone(computer, "Volume display");
        Gone(world, "Holodeck");
        Transform holodeck = new GameObject("Holodeck").transform;
        holodeck.SetParent(world, false);
        Material sceneMat = Mat("GpuHolodeck", "ShaderEmu/GpuHolodeck");
        Material volumeMat = AssetDatabase.LoadAssetAtPath<Material>(Generated + "/GpuVolume.mat");
        if (volumeMat != null) sceneMat.SetTexture("_State", volumeMat.GetTexture("_State"));
        sceneMat.SetVector("_ScreenSize", new Vector4(1.28f, 0.8f, 0, 0));
        GiveRom(sceneMat);
        sceneMat.renderQueue = 2503;   // one later than its shader says: the room's own things are marked between the mask and it (HoloGuards)

        Transform content = new GameObject("Holodeck content").transform;
        content.SetParent(holodeck, false);
        // (a model's own objects cannot be moved out of it: copies, and the model's put away)
        Transform maskModel = models.Find("Holodeck mask");
        Transform mask = Object.Instantiate(maskModel.gameObject, content, false).transform;
        mask.name = "Holodeck mask";
        mask.localPosition = Vector3.zero;
        mask.localRotation = Quaternion.identity;
        maskModel.gameObject.SetActive(false);
        GameObject seal = Object.Instantiate(mask.gameObject, content, false);
        seal.name = "Holodeck seal";
        Material maskMat = Mat("HolodeckMask", "ShaderEmu/HolodeckMask");
        if (volumeMat != null) maskMat.SetTexture("_State", volumeMat.GetTexture("_State"));
        mask.GetComponent<MeshRenderer>().sharedMaterial = maskMat;
        Material sealMat = Mat("VolumeSeal", "ShaderEmu/VolumeSeal");
        sealMat.renderQueue = 2504;
        seal.GetComponent<MeshRenderer>().sharedMaterial = sealMat;
        foreach (MeshRenderer renderer in content.GetComponentsInChildren<MeshRenderer>())
        {
            GameObjectUtility.SetStaticEditorFlags(renderer.gameObject, 0);
            renderer.shadowCastingMode = ShadowCastingMode.Off;
            renderer.receiveShadows = false;
            renderer.lightProbeUsage = LightProbeUsage.Off;
            renderer.reflectionProbeUsage = ReflectionProbeUsage.Off;
        }

        Mesh points = LoadOrCreate(Generated + "/HolodeckPoints.asset", () =>
        {
            int[] indices = new int[GpuTriangles];
            for (int i = 0; i < GpuTriangles; i++) indices[i] = i;
            Mesh mesh = new Mesh { name = "HolodeckPoints", indexFormat = IndexFormat.UInt32 };
            mesh.vertices = new Vector3[GpuTriangles];
            mesh.SetIndices(indices, MeshTopology.Points, 0, false);
            return mesh;
        });
        points.bounds = new Bounds(Vector3.zero, Vector3.one * 2000f);   // the world reaches where it will
        EditorUtility.SetDirty(points);
        GameObject scene = new GameObject("Holodeck scene", typeof(MeshFilter), typeof(MeshRenderer));
        scene.transform.SetParent(holodeck, false);
        scene.GetComponent<MeshFilter>().sharedMesh = points;
        MeshRenderer sceneRenderer = scene.GetComponent<MeshRenderer>();
        sceneRenderer.sharedMaterial = sceneMat;
        sceneRenderer.shadowCastingMode = ShadowCastingMode.Off;
        sceneRenderer.receiveShadows = false;
        sceneRenderer.lightProbeUsage = LightProbeUsage.Off;
        sceneRenderer.reflectionProbeUsage = ReflectionProbeUsage.Off;

        // ---- the one behaviour that knows the rooms, the controller's, and the voices'
        EmuMachine machine = null;
        foreach (EmuMachine m in world.GetComponentsInChildren<EmuMachine>(true)) machine = m;
        UdonSharpEditorUtility.CopyUdonToProxy(machine);
        EmuHolodeck deck = Udon<EmuHolodeck>(holodeck.gameObject);
        EmuGamepad gamepad = Udon<EmuGamepad>(new GameObject("Controller"));
        gamepad.transform.SetParent(holodeck, false);
        EmuVoices voices = Udon<EmuVoices>(new GameObject("Voices"));
        voices.transform.SetParent(holodeck, false);
        voices.low = new Vector3[rooms];
        voices.high = new Vector3[rooms];
        deck.machine = machine;
        deck.gamepad = gamepad;
        deck.content = content;
        deck.scene = scene.transform;
        // off until somebody sits down (EmuHolodeck.Entered): left on, the mask stood in the den as a black box
        content.gameObject.SetActive(false);
        scene.SetActive(false);

        deck.sceneMaterial = sceneMat;
        deck.roomSize = new Vector3(side, tall, side);
        deck.screenAt = new Vector3(plan.screen[0], plan.screen[1], plan.screen[2]);
        deck.screenSize = new Vector3(plan.screen[3], plan.screen[4], 1f);
        deck.screenTurn = new Vector3(plan.screenTurn[0], plan.screenTurn[1], 0);
        LargeScreen(deck, plan.screen[3] / plan.screen[4], side);
        deck.blackTexture = machine.blackTexture;
        deck.stations = new VRC.SDKBase.VRCStation[rooms];
        deck.frames = new Transform[rooms];
        deck.eyes = new Transform[rooms];
        deck.screens = new Transform[rooms];
        deck.screenRenderers = new Renderer[rooms];
        deck.screenMaterials = new Material[rooms];
        deck.doorLabels = new TextMeshProUGUI[rooms];
        deck.plateLabels = new TextMeshProUGUI[rooms];
        deck.scaleLabels = new TextMeshProUGUI[rooms];
        Collider[] screenPlates = new Collider[rooms];
        Color back = new Color(0.05f, 0.055f, 0.07f), dim = new Color(0.62f, 0.68f, 0.76f);
        TextMeshProUGUI unused;
        List<KeyValuePair<Button, string>> keys = new List<KeyValuePair<Button, string>>();
        List<EmuSeat> seats = new List<EmuSeat>();
        for (int k = 0; k < rooms; k++)
        {
            Transform frame = new GameObject("Holodeck " + (k + 1)).transform;
            frame.SetParent(holodeck, false);
            frame.localPosition = new Vector3(plan.rooms[k * 3], 0, plan.rooms[k * 3 + 1]);
            frame.localEulerAngles = new Vector3(0, plan.rooms[k * 3 + 2], 0);
            deck.frames[k] = frame;
            // what stops a visitor: the room's floor, ceiling and three walls (its door wall is the corridor's)
            Solid(frame, "Floor", -side / 2, side / 2, -0.1f, 0, 0, side);
            Solid(frame, "Ceiling", -side / 2, side / 2, tall, tall + 0.1f, 0, side);
            Solid(frame, "Wall", -side / 2 - wall, -side / 2, 0, tall, 0, side);
            Solid(frame, "Wall", side / 2, side / 2 + wall, 0, tall, 0, side);
            Solid(frame, "Wall", -side / 2, side / 2, 0, tall, side, side + wall);
            Vector3 a = frame.TransformPoint(new Vector3(-side / 2, 0, 0)), b = frame.TransformPoint(new Vector3(side / 2, tall, side));
            voices.low[k] = world.TransformPoint(Vector3.Min(world.InverseTransformPoint(a), world.InverseTransformPoint(b)));
            voices.high[k] = world.TransformPoint(Vector3.Max(world.InverseTransformPoint(a), world.InverseTransformPoint(b)));
            // (under Decor, as every lamp the bake takes is: under its room's frame it was left out, and the room was black)
            BakedLight(decor, "Holodeck light", decor.InverseTransformPoint(frame.TransformPoint(new Vector3(0, tall - 0.4f, side / 2))),
                       new Color(1f, 0.9f, 0.72f), 2.0f, 6f);

            // the seat: a station of the scene, which VRChat syncs by itself, one visitor at a time
            GameObject seat = new GameObject("Seat");
            seat.transform.SetParent(frame, false);
            // (on the floor while nobody sits: it rises under a sitter as their pose sinks, EmuSeat)
            seat.transform.localPosition = new Vector3(0, 0, plan.seat);
            BoxCollider reach = seat.AddComponent<BoxCollider>();
            reach.size = new Vector3(0.5f, 0.7f, 0.5f);
            reach.center = new Vector3(0, 0.15f + SeatHeight, 0);
            reach.isTrigger = true;
            Transform leave = new GameObject("Seat exit").transform;
            leave.SetParent(frame, false);
            leave.localPosition = new Vector3(-0.8f, 0, plan.seat);
            VRC.SDK3.Components.VRCStation station = seat.AddComponent<VRC.SDK3.Components.VRCStation>();
            station.PlayerMobility = VRC.SDKBase.VRCStation.Mobility.Immobilize;
            station.seated = true;
            station.disableStationExit = true;   // the sticks are the game's: EmuHolodeck stands the sitter up when they jump
            station.canUseStationFromStation = true;
            station.stationEnterPlayerLocation = seat.transform;
            station.stationExitPlayerLocation = leave;
            deck.stations[k] = station;
            EmuSeat sits = Udon<EmuSeat>(seat);
            sits.holodeck = deck;
            sits.room = k;
            sits.height = SeatHeight;
            seats.Add(sits);
            Transform eye = new GameObject("Seat eye").transform;
            eye.SetParent(frame, false);
            eye.localPosition = new Vector3(0, SeatHeight + 0.7f, plan.seat);
            deck.eyes[k] = eye;

            // the screen: this room's sitter's display
            GameObject screen = GameObject.CreatePrimitive(PrimitiveType.Quad);
            screen.name = "Screen";
            Object.DestroyImmediate(screen.GetComponent<Collider>());
            screen.transform.SetParent(frame, false);
            screen.transform.localPosition = deck.screenAt;
            screen.transform.localEulerAngles = new Vector3(plan.screenTurn[0], plan.screenTurn[1], 0);
            screen.transform.localScale = new Vector3(plan.screen[3], plan.screen[4], 1f);
            Material screenMat = Mat("HoloScreen " + k, "ShaderEmu/DisplayShow");
            screenMat.SetFloat("_Aspect", plan.screen[3] / plan.screen[4]);
            screenMat.SetFloat("_Glow", 1.2f);
            screenMat.SetVector("_Size", Vector4.zero);
            MeshRenderer screenRenderer = screen.GetComponent<MeshRenderer>();
            screenRenderer.sharedMaterial = screenMat;
            screenRenderer.shadowCastingMode = ShadowCastingMode.Off;
            screenRenderer.receiveShadows = false;
            BoxCollider plate = screen.AddComponent<BoxCollider>();
            plate.size = new Vector3(1f, 1f, 0.01f);
            plate.isTrigger = true;
            screenPlates[k] = plate;
            deck.screens[k] = screen.transform;
            deck.screenRenderers[k] = screenRenderer;
            deck.screenMaterials[k] = screenMat;

            ConsolePanel(frame, k, new Vector3(plan.panel[0], plan.panel[1], plan.panel[2]), new Vector3(plan.screenTurn[0], plan.screenTurn[1], 0), deck, keys);

            // (no controllers lie at the console: one taken with its keys appears in the sitter's hand)

            // by its door, on the corridor's side: whose it is
            RectTransform name = Panel(frame, "Holodeck " + (k + 1) + " name", new Vector3(doorWide / 2 + 0.42f, 1.45f, -wall - 0.014f), Vector3.zero, 320, 180, back);
            Object.DestroyImmediate(name.GetComponent<VRC.SDK3.Components.VRCUiShape>());
            Object.DestroyImmediate(name.GetComponent<BoxCollider>());
            Object.DestroyImmediate(name.GetComponent<GraphicRaycaster>());
            Label(name, "Title", "HOLODECK " + (k + 1), 10, 10, 300, 60, 34, TextAnchor.MiddleCenter, Color.white);
            deck.doorLabels[k] = Label(name, "Who", "FREE", 10, 80, 300, 80, 30, TextAnchor.MiddleCenter, new Color(1f, 0.8f, 0.3f));
        }

        // ---- over the hand that holds a controller: what it is, and how to put it back
        RectTransform notice = Panel(holodeck, "Controller notice", Vector3.zero, Vector3.zero, 340, 96, new Color(0.03f, 0.03f, 0.04f, 0.85f));
        Object.DestroyImmediate(notice.GetComponent<VRC.SDK3.Components.VRCUiShape>());
        Object.DestroyImmediate(notice.GetComponent<BoxCollider>());
        Object.DestroyImmediate(notice.GetComponent<GraphicRaycaster>());
        gamepad.noticeLabel = Label(notice, "Text", "", 8, 4, 324, 70, 22, TextAnchor.MiddleCenter, Color.white);
        RectTransform bar = Child(notice, "Bar", 8, 78, 324, 12);
        bar.pivot = new Vector2(0, 1);
        bar.anchoredPosition = new Vector2(8, -78);
        Plate(bar, new Color(1f, 0.6f, 0.1f)).raycastTarget = false;
        gamepad.notice = notice;
        gamepad.noticeBar = bar;
        notice.gameObject.SetActive(false);   // until a hand holds a controller: it stood at the room's middle, on the floor
        gamepad.machine = machine;
        gamepad.keys = machine.gpuKeyboard;

        foreach (EmuShareHub hub in world.GetComponentsInChildren<EmuShareHub>(true)) { deck.hub = hub; gamepad.hub = hub; }
        foreach (EmuStreams streams in world.GetComponentsInChildren<EmuStreams>(true)) deck.streams = streams;
        foreach (EmuPointer pointer in world.GetComponentsInChildren<EmuPointer>(true))
        {
            UdonSharpEditorUtility.CopyUdonToProxy(pointer);
            pointer.holoScreens = screenPlates;
            pointer.holoAspect = plan.screen[3] / plan.screen[4];
            Apply(pointer);
            deck.pointer = pointer;
            gamepad.pointer = pointer;
        }
        Apply(gamepad);
        Apply(voices);
        ConsoleBeams(world, deck);
        Apply(deck);
        foreach (EmuSeat sits in seats)
        {
            Apply(sits);
            UdonBehaviour udon = UdonSharpEditorUtility.GetBackingUdonBehaviour(sits);
            udon.interactText = "Sit down";
            EditorUtility.SetDirty(udon);
        }
        foreach (KeyValuePair<Button, string> key in keys) OnClick(key.Key, deck, key.Value);

        NoControllerModels(world);

        machine.volumeMaterial = sceneMat;
        machine.volumeMaskMaterial = maskMat;
        Apply(machine);
        HoloGuards(world);
    }

    static void LightVolumeIn(Transform world, string name, Vector3 centre, Vector3 size)
    {
        GameObject holder = new GameObject(name);
        holder.transform.SetParent(world, false);
        holder.transform.localPosition = centre;
        holder.transform.localScale = size;
        VRCLightVolumes.LightVolume volume = holder.AddComponent<VRCLightVolumes.LightVolume>();
        volume.Bake = true;
        volume.AdaptiveResolution = false;
        volume.VoxelsPerUnit = 2f;
        volume.Recalculate();
    }

    static void ProbeIn(Transform decor, string name, Vector3 centre, Vector3 size)
    {
        ReflectionProbe probe = new GameObject(name).AddComponent<ReflectionProbe>();
        probe.transform.SetParent(decor, false);
        probe.transform.localPosition = centre;
        probe.mode = ReflectionProbeMode.Baked;
        probe.size = size;
        probe.boxProjection = true;
        probe.resolution = 256;
        probe.hdr = true;
    }
}
