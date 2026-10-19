using System.IO;
using TMPro;
using UdonSharpEditor;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.UI;
using VRC.Udon;

// The second machine model (experiments/udon_aot): the kernel translated to Udon, its program from
// build/udon_linux, its driver EmuUdonCpu on the control panel beside the shader machine's.
public static partial class ShaderEmuBuilder
{
    [MenuItem("ShaderEmu/Add the Udon machine to the open scene")]
    public static void AddUdonCpu()
    {
        EmuMachine machine = Object.FindObjectOfType<EmuMachine>();
        GameObject panelObject = GameObject.Find("Control panel");
        if (machine == null || panelObject == null) throw new System.Exception("no machine in the open scene: run ShaderEmu/Build world");
        string dir = ShaderEmuImages.Repo + "/build/udon_linux";
        if (!File.Exists(dir + "/prog.syms")) throw new System.Exception("no translated kernel: run experiments/udon_aot/l2u/l2u.exe into build/udon_linux");
        CreateProgramAssets();
        UdonSharpEditorUtility.CopyUdonToProxy(machine);
        Transform world = machine.transform.parent;
        RectTransform panel = panelObject.GetComponent<RectTransform>();
        Gone(world, "Udon machine");
        Gone(panel, "Udon CPU");
        Gone(panel, "Udon CPU status");

        UdonAotProgramAsset program = LoadOrCreate(Generated + "/UdonLinux.asset", () => ScriptableObject.CreateInstance<UdonAotProgramAsset>());
        program.RefreshProgram();
        EditorUtility.SetDirty(program);

        GameObject root = new GameObject("Udon machine");
        root.transform.SetParent(world, false);
        GameObject cpuObject = new GameObject("Udon CPU program");
        cpuObject.transform.SetParent(root.transform, false);
        UdonBehaviour cpu = cpuObject.AddComponent<UdonBehaviour>();
        cpu.programSource = program;
        SerializedObject so = new SerializedObject(cpu);
        so.FindProperty("serializedProgramAsset").objectReferenceValue = program.SerializedProgramAsset;
        so.ApplyModifiedPropertiesWithoutUndo();

        EmuUdonCpu drv = Udon<EmuUdonCpu>(root);
        drv.cpu = cpu;
        drv.machine = machine;
        drv.terminal = machine.terminal;
        drv.consoleKeyboard = machine.consoleKeyboard;
        drv.ramLanes = new Texture2D[] { machine.imageTextures[0], machine.imageTextures[1], machine.imageTextures[2], machine.imageTextures[3] };
        drv.romLanes = new Texture2D[] { machine.imageTextures[4], machine.imageTextures[5], machine.imageTextures[6], machine.imageTextures[7] };
        drv.unpackMaterial = Mat("UdonAotUnpack", "ShaderEmu/UdonAotUnpack");
        drv.unpackTarget = Target("UdonAotUnpack", 2048, 2048, 0);
        drv.stageSmall = Stage("UdonAotStage64", 64);
        drv.stageLarge = Stage("UdonAotStage256", 256);
        drv.packMaterial = Mat("UdonAotPack", "ShaderEmu/UdonAotPack");
        drv.udonState = StateTexture("UdonAotState", 2048, 256);
        drv.displayMaterial = Mat("UdonAotDisplay", "ShaderEmu/Display");
        drv.displayMaterial.CopyPropertiesFromMaterial(machine.displayMaterial);
        drv.displayMaterial.SetFloat("_StateRow", -(0x700000 / 2048));   // the gathered copy's row 0 is 0x87000000
        EditorUtility.SetDirty(drv.displayMaterial);
        drv.soundStage = Stage("UdonAotSoundStage", 64);
        drv.soundState = StateTexture("UdonAotSoundState", 2048, 64);
        drv.soundControl = Mat("UdonAotSoundControl", "ShaderEmu/Sound");
        drv.gpuState = StateTexture("UdonAotGpuState", 2048, 1024);
        drv.gpuPackMaterial = Mat("UdonAotGpuPack", "ShaderEmu/UdonAotPack");
        drv.gpuPackMaterial.SetFloat("_Keep", 1);
        EditorUtility.SetDirty(drv.gpuPackMaterial);
        drv.gpuBackMaterial = Mat("UdonAotGpuBack", "ShaderEmu/UdonAotGpuBack");
        RenderTexture gpuTarget = machine.gpuCamera.targetTexture;
        drv.gpuBack = Target("UdonAotGpuBack", gpuTarget.width, gpuTarget.height, 0);
        drv.fetchMaterial = Mat("UdonAotFetch", "ShaderEmu/UdonAotFetch");
        drv.fetchTarget = Target("UdonAotFetch", 256, 256, 0);
        foreach (string line in File.ReadAllLines(dir + "/prog.syms"))
        {
            string[] p = line.Split(' ');
            if (p[0] == "@textwords") drv.textWords = int.Parse(p[1]);
            if (p[0] == "@textsum") drv.textSum = int.Parse(p[1]);
        }
        Button power = MakeButton(panel, "Udon CPU", "Udon CPU on", 1060, 440, 190, 80, 28, out drv.powerLabel);
        drv.label = Label(panel, "Udon CPU status", "Udon CPU: off", 1265, 440, 400, 80, 22, TextAnchor.MiddleLeft, new Color(0.85f, 0.87f, 0.9f));
        Apply(drv);
        OnClick(power, drv, "Power");
        RetroNewPanels();
        AssetDatabase.SaveAssets();
        EditorSceneManager.MarkSceneDirty(machine.gameObject.scene);
        Debug.Log("[ShaderEmu] Udon machine added: " + program.SerializedProgramAsset.GetSerializedProgramSize() + " bytes of program");
    }

    // A word a pixel, 8192 a row: one row of the state texture's texels
    static Texture2D Stage(string name, int rows)
    {
        Texture2D t = LoadOrCreate(Generated + "/" + name + ".asset", () => new Texture2D(8192, rows, TextureFormat.RGBA32, false, true));
        t.filterMode = FilterMode.Point;
        t.wrapMode = TextureWrapMode.Clamp;
        EditorUtility.SetDirty(t);
        return t;
    }
}
