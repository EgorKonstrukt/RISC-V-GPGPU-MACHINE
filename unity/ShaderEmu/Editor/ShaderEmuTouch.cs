using System.Collections.Generic;
using TMPro;
using UdonSharp;
using UdonSharpEditor;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.UI;

// Physical buttons (docs/world/world.md, "Physical buttons"): a switch on the control panel, and
// the panels whose buttons and sliders a fingertip presses (EmuPointer.Touch).
public static partial class ShaderEmuBuilder
{
    // The control panel's words under the read-out, beside the switch.
    public const string PanelHelp =
        "Your machine is a computer emulated in a pixel shader on your own graphics card; every player runs their own, and can show you theirs.\n" +
        "Each hand has a beam. On the big screen it is the mouse: trigger = left, grip = right, right stick = wheel. " +
        "On a keyboard a key stays down while the trigger is held; Shift, Ctrl and Alt latch.\n" +
        "Physical buttons ON: a fingertip presses keys, buttons and sliders. Steady pointer ON: the beam's mouse is smoothed and a click holds still.\n" +
        "Desktop: 'Use my keyboard' types with the real one.";

    // The switches beside the panel's words, which they narrow.
    static void PhysicalSwitch(RectTransform panel, EmuPointer pointer)
    {
        Gone(panel, "Physical buttons");
        Gone(panel, "Steady pointer");
        Transform help = panel.Find("Help");
        if (help != null)
        {
            RectTransform rect = (RectTransform)help;
            rect.sizeDelta = new Vector2(800, 170);
            TextMeshProUGUI words = help.GetComponent<TextMeshProUGUI>();
            words.text = PanelHelp;
            words.fontSize = 21;
            EditorUtility.SetDirty(words);
        }
        TextMeshProUGUI label;
        Button toggle = MakeButton(panel, "Physical buttons", "Physical buttons: OFF", 850, 596, 180, 72, 22, out label);
        pointer.physicalLabel = label;
        OnClick(toggle, pointer, "PhysicalButtons");
        // and the pointer's smoothing and click tolerance, under it (on unless turned off)
        Button steady = MakeButton(panel, "Steady pointer", "Steady pointer: ON", 850, 676, 180, 72, 22, out label);
        pointer.steadyLabel = label;
        OnClick(steady, pointer, "SteadyPointer");
    }

    // Every panel with buttons or sliders a beam's laser presses, but the keyboards (a finger
    // presses those as keys): its collider, and each button's and slider's rectangle.
    static void TouchPanels(Transform world, EmuPointer pointer)
    {
        List<Collider> panels = new List<Collider>();
        List<Button> buttons = new List<Button>();
        List<RectTransform> buttonRects = new List<RectTransform>();
        List<int> buttonPanel = new List<int>();
        List<Slider> sliders = new List<Slider>();
        List<RectTransform> sliderRects = new List<RectTransform>();
        List<int> sliderPanel = new List<int>();
        foreach (Selectable control in world.GetComponentsInChildren<Selectable>(true))
        {
            if (!(control is Button) && !(control is Slider)) continue;
            if (control.GetComponentInParent<EmuKeyboard>(true) != null) continue;
            Canvas canvas = control.GetComponentInParent<Canvas>(true);
            while (canvas != null && canvas.GetComponent<BoxCollider>() == null && canvas.transform.parent != null)
                canvas = canvas.transform.parent.GetComponentInParent<Canvas>(true);
            BoxCollider box = canvas != null ? canvas.GetComponent<BoxCollider>() : null;
            if (box == null) continue;
            int at = panels.IndexOf(box);
            if (at < 0) { at = panels.Count; panels.Add(box); }
            if (control is Button) { buttons.Add((Button)control); buttonRects.Add((RectTransform)control.transform); buttonPanel.Add(at); }
            else { sliders.Add((Slider)control); sliderRects.Add((RectTransform)control.transform); sliderPanel.Add(at); }
        }
        pointer.touchPanels = panels.ToArray();
        pointer.touchButtons = buttons.ToArray();
        pointer.touchButtonRects = buttonRects.ToArray();
        pointer.touchButtonPanel = buttonPanel.ToArray();
        pointer.touchSliders = sliders.ToArray();
        pointer.touchSliderRects = sliderRects.ToArray();
        pointer.touchSliderPanel = sliderPanel.ToArray();
        Debug.Log("[ShaderEmu] physical buttons: " + buttons.Count + " buttons and " + sliders.Count + " sliders on " + panels.Count + " panels");
    }

    // The 18 October 2026 changes to a built scene: the classroom towers' speed sliders, the
    // control panel's switch for physical buttons and its words. No bake: the new plates take the
    // probes' light, the control panel's is drawn again in place.
    [MenuItem("ShaderEmu/Add physical buttons and the classroom's speed sliders to the open scene")]
    public static void AddTouchAndSpeeds()
    {
        GameObject root = GameObject.Find("ShaderEmu");
        EmuStations stations = Object.FindObjectOfType<EmuStations>();
        EmuPointer pointer = Object.FindObjectOfType<EmuPointer>();
        if (root == null || stations == null || pointer == null) { Debug.LogError("[ShaderEmu] no world in the open scene: run ShaderEmu/Build world"); return; }
        Transform world = root.transform;
        UdonSharpEditorUtility.CopyUdonToProxy(stations);
        UdonSharpEditorUtility.CopyUdonToProxy(pointer);
        StationsPlan plan = ReadStations();
        int count = plan.places.Length;
        stations.speedSliders = new Slider[count];
        for (int s = 0; s < count; s++)
        {
            Transform place = stations.transform.Find("Station " + s);
            EmuPlace own = place != null ? place.GetComponent<EmuPlace>() : null;
            if (own == null) { Debug.LogError("[ShaderEmu] no Station " + s); continue; }
            UdonSharpEditorUtility.CopyUdonToProxy(own);
            stations.speedSliders[s] = StationSpeed(place, s, own, plan);
            Apply(own);
        }
        Apply(stations);
        RectTransform panel = (RectTransform)world.Find("Computer/Control panel");
        PhysicalSwitch(panel, pointer);
        TouchPanels(world, pointer);
        Apply(pointer);
        RetroNewPanels();
        RetroInPlace(world, new List<Canvas> { panel.GetComponent<Canvas>() });
        EditorSceneManager.MarkSceneDirty(root.scene);
        Debug.Log("[ShaderEmu] physical buttons and the towers' speed sliders are in the scene (not saved)");
    }

    // The towers' speed sliders to 256k, their reset key alone (the turbo key left the model,
    // world/bake_pc.py) and a volume slider on each monitor, into a built scene. No bake.
    [MenuItem("ShaderEmu/Add the monitors' volume sliders and the towers' new speeds to the open scene")]
    public static void AddVolumesAndSpeeds()
    {
        GameObject root = GameObject.Find("ShaderEmu");
        EmuStations stations = Object.FindObjectOfType<EmuStations>();
        EmuPointer pointer = Object.FindObjectOfType<EmuPointer>();
        if (root == null || stations == null || pointer == null) { Debug.LogError("[ShaderEmu] no world in the open scene: run ShaderEmu/Build world"); return; }
        UdonSharpEditorUtility.CopyUdonToProxy(stations);
        UdonSharpEditorUtility.CopyUdonToProxy(pointer);
        StationsPlan plan = ReadStations();
        int count = plan.places.Length;
        stations.speedSliders = new Slider[count];
        stations.volumeSliders = new Slider[count];
        for (int s = 0; s < count; s++)
        {
            Transform place = stations.transform.Find("Station " + s);
            EmuPlace own = place != null ? place.GetComponent<EmuPlace>() : null;
            if (own == null) { Debug.LogError("[ShaderEmu] no Station " + s); continue; }
            UdonSharpEditorUtility.CopyUdonToProxy(own);
            stations.speedSliders[s] = StationSpeed(place, s, own, plan);
            stations.volumeSliders[s] = StationVolume(place, s, own, plan);
            TowerHotKeys(place, s, own, plan);
            Apply(own);
        }
        Apply(stations);
        RetroNewPanels();
        TouchPanels(root.transform, pointer);
        Apply(pointer);
        EditorSceneManager.MarkSceneDirty(root.scene);
        Debug.Log("[ShaderEmu] the monitors' volume sliders and the towers' speeds are in the scene (not saved)");
    }

    // The pointer's two switches on every classroom monitor, beside its volume (EmuPointer). No bake.
    [MenuItem("ShaderEmu/Add the monitors' pointer switches to the open scene")]
    public static void AddPointerSwitches()
    {
        GameObject root = GameObject.Find("ShaderEmu");
        EmuStations stations = Object.FindObjectOfType<EmuStations>();
        EmuPointer pointer = Object.FindObjectOfType<EmuPointer>();
        if (root == null || stations == null || pointer == null) { Debug.LogError("[ShaderEmu] no world in the open scene: run ShaderEmu/Build world"); return; }
        UdonSharpEditorUtility.CopyUdonToProxy(pointer);
        StationsPlan plan = ReadStations();
        List<TextMeshProUGUI> physical = new List<TextMeshProUGUI>(), steady = new List<TextMeshProUGUI>();
        for (int s = 0; s < plan.places.Length; s++)
        {
            Transform place = stations.transform.Find("Station " + s);
            EmuPlace own = place != null ? place.GetComponent<EmuPlace>() : null;
            if (own == null) { Debug.LogError("[ShaderEmu] no Station " + s); continue; }
            StationSwitches(place, s, own, plan, physical, steady);
        }
        pointer.physicalLabels = physical.ToArray();
        pointer.steadyLabels = steady.ToArray();
        Apply(pointer);
        RetroNewPanels();   // after: a label no behaviour names is printed into the plate
        TouchPanels(root.transform, pointer);
        Apply(pointer);
        EditorSceneManager.MarkSceneDirty(root.scene);
        Debug.Log("[ShaderEmu] the monitors' pointer switches are in the scene (not saved)");
    }

    // A held key's cap sinks (EmuKeyboard.Sink, Plate.shader): the keyboards' caps' depths, into a built scene.
    [MenuItem("ShaderEmu/Sink held keys' caps in the open scene")]
    public static void AddSinkingCaps()
    {
        EmuStations stations = Object.FindObjectOfType<EmuStations>();
        if (stations == null) { Debug.LogError("[ShaderEmu] no world in the open scene: run ShaderEmu/Build world"); return; }
        foreach (EmuKeyboard keyboard in Object.FindObjectsOfType<EmuKeyboard>(true))
        {
            UdonSharpEditorUtility.CopyUdonToProxy(keyboard);
            if (System.Array.IndexOf(stations.keyboards, keyboard) >= 0) StationCaps(keyboard);
            else DenCaps(keyboard);
            Apply(keyboard);
        }
        EditorSceneManager.MarkSceneDirty(stations.gameObject.scene);
        Debug.Log("[ShaderEmu] held keys' caps sink (not saved)");
    }
}
