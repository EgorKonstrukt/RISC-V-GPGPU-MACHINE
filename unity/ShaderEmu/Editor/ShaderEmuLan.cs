// The network's panel (docs/lan.md): on the right wall beside the danger zone, every visitor's
// machine with its address and this visitor's own machine's counts. A canvas: no bake.
using TMPro;
using UdonSharpEditor;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;

public static partial class ShaderEmuBuilder
{
    static void Lan(Transform world, EmuMachine machine)
    {
        Gone(world, "Network panel");
        Gone(world, "Lan");
        Color back = new Color(0.04f, 0.055f, 0.07f), dim = new Color(0.62f, 0.68f, 0.76f), amber = new Color(1f, 0.8f, 0.3f);
        const float width = 760, height = 600;
        // (the danger zone is 760 wide at z 3.5: this hangs at its left, the same height)
        RectTransform panel = Panel(world, "Network panel", new Vector3(4.44f, 1.47f, 2.70f), new Vector3(0, 90, 0), width, height, back);
        EmuLan lan = Udon<EmuLan>(new GameObject("Lan"));
        lan.transform.SetParent(world, false);
        lan.machine = machine;
        lan.hub = Object.FindObjectOfType<EmuShareHub>();
        Label(panel, "Title", "NETWORK", 14, 12, 300, 44, 34, TextAnchor.MiddleLeft, Color.white);
        Label(panel, "Help", "Every machine that is on is on one network, 10.0.0.0/16: ping, telnet, Doom and Quake between them.", 14, 56, width - 28, 50, 17,
              TextAnchor.UpperLeft, dim);
        Label(panel, "Own title", "Your machine", 14, 110, width - 28, 24, 18, TextAnchor.MiddleLeft, dim);
        lan.ownLabel = Label(panel, "Own", "", 14, 136, width - 28, 96, 16, TextAnchor.UpperLeft, amber);
        Label(panel, "List title", "Everybody's (a sending is a batch of packets)", 14, 238, width - 28, 24, 18, TextAnchor.MiddleLeft, dim);
        lan.listLabel = Label(panel, "List", "", 14, 264, width - 28, 326, 18, TextAnchor.UpperLeft, amber);
        Apply(lan);
    }

    [MenuItem("ShaderEmu/Add the network's panel to the open scene")]
    public static void AddLan()
    {
        EmuMachine machine = Object.FindObjectOfType<EmuMachine>();
        if (machine == null) throw new System.Exception("no machine in the open scene: run ShaderEmu/Build world");
        CreateProgramAssets();
        UdonSharpEditorUtility.CopyUdonToProxy(machine);
        Lan(machine.transform.parent, machine);
        RetroNewPanels();
        AssetDatabase.SaveAssets();
        EditorSceneManager.MarkSceneDirty(machine.gameObject.scene);
        Debug.Log("[ShaderEmu] the network's panel is on the right wall (the scene is not saved)");
    }

    // The control panel's read-out has the registers under it: room for seventeen lines.
    static void StatsRoom(Transform world)
    {
        foreach (TextMeshProUGUI label in world.GetComponentsInChildren<TextMeshProUGUI>(true))
        {
            if (label.name != "Stats" || label.transform.parent.name != "Control panel") continue;
            RectTransform rect = label.rectTransform;
            rect.sizeDelta = new Vector2(rect.sizeDelta.x, 486);
            label.fontSize = 22;
            label.enableWordWrapping = false;
            EditorUtility.SetDirty(label);
            // (its sunken field, if the panel is styled: the label's rectangle and ten more each way)
            Transform field = label.transform.parent.Find("Stats field");
            if (field != null) ((RectTransform)field).sizeDelta = rect.sizeDelta + new Vector2(20, 20);
        }
    }

    // A baked scene from before 12 October 2026, without building the world again: the streams'
    // sixteen slots and the players' panel made again, the consoles of the holodecks, the
    // network's panel, the read-out's room. Run it after "Put the modelled room into the open
    // scene" (the classroom is that command's); the plates it makes want a bake.
    [MenuItem("ShaderEmu/Bring the open scene up to 12 October")]
    public static void October12()
    {
        EmuMachine machine = Object.FindObjectOfType<EmuMachine>();
        if (machine == null) throw new System.Exception("no machine in the open scene: run ShaderEmu/Build world");
        Transform world = machine.transform.parent;
        AddSharing();
        UdonSharpEditorUtility.CopyUdonToProxy(machine);
        Lan(world, machine);
        StatsRoom(world);
        ConsolesAgain();
        Canvas control = GameObject.Find("Control panel").GetComponent<Canvas>();
        Transform plate = control.transform.Find("Control panel plate");
        if (plate != null) Object.DestroyImmediate(plate.gameObject);   // drawn again, for the read-out's larger field
        RetroNewPanels();
        AssetDatabase.SaveAssets();
        EditorSceneManager.MarkSceneDirty(machine.gameObject.scene);
        Debug.Log("[ShaderEmu] the scene has the 12 October fixes (not saved; its new plates want a bake)");
    }
}
