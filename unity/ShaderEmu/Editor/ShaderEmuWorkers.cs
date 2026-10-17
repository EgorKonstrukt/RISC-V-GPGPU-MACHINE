using TMPro;
using UdonSharpEditor;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.UI;

// The workers' layout, on the wall under the control panel's keys (docs/machine/multicore.md, "Packing
// the small workers"): geotune's own pick or a preset, and what it picked and runs.
public static partial class ShaderEmuBuilder
{
    static void WorkersPanel(Transform computer, EmuMachine machine)
    {
        Transform old = computer.Find("Workers panel");
        if (old != null) Object.DestroyImmediate(old.gameObject);
        // under the control panel's keys (x 1,060 to 1,660 of its 1,700 mm) and the rail below it, 3 cm off the wall
        Vector3 control = computer.Find("Control panel").localPosition;
        RectTransform panel = Panel(computer, "Workers panel", control + new Vector3(0.51f, -0.60f, 0.06f), Vector3.zero, 640, 170,
                                    new Color(0.05f, 0.055f, 0.07f));
        Label(panel, "Title", "Worker cores", 20, 10, 600, 30, 24, TextAnchor.MiddleLeft, new Color(0.62f, 0.68f, 0.76f));
        string[] names = { "Auto", "Dense", "Medium", "Wide" };
        string[] methods = { "WorkersAuto", "WorkersDense", "WorkersMedium", "WorkersWide" };
        machine.workerPlates = new Image[names.Length];
        TextMeshProUGUI unused;
        for (int i = 0; i < names.Length; i++)
        {
            Button key = MakeButton(panel, "Workers " + names[i], names[i], 20 + 150 * i, 46, 140, 56, 24, out unused);
            machine.workerPlates[i] = key.targetGraphic as Image;
            OnClick(key, machine, methods[i]);
        }
        machine.workersLabel = Label(panel, "Workers", "Auto: measured at power-on", 20, 110, 600, 52, 18, TextAnchor.UpperLeft, Color.white);
        Apply(machine);
    }

    // Into the scene as it is, without building the world again (which would need a bake).
    [MenuItem("ShaderEmu/Add the workers panel to the open scene")]
    public static void AddWorkers()
    {
        EmuMachine machine = Object.FindObjectOfType<EmuMachine>();
        Transform computer = Den();
        if (machine == null || computer == null) throw new System.Exception("no machine in the open scene: run ShaderEmu/Build world");
        CreateProgramAssets();
        UdonSharpEditorUtility.CopyUdonToProxy(machine);
        WorkersPanel(computer, machine);
        RetroNewPanels();   // never left as a bare canvas
        AssetDatabase.SaveAssets();
        EditorSceneManager.MarkSceneDirty(machine.gameObject.scene);
        Debug.Log("[ShaderEmu] the workers panel is under the control panel (the scene is not saved)");
    }
}
