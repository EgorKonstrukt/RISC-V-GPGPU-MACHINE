using TMPro;
using UdonSharp;
using UnityEngine;
using VRC.SDK3.Image;
using VRC.SDK3.Persistence;
using VRC.SDK3.Rendering;
using VRC.SDK3.StringLoading;
using VRC.SDKBase;
using VRC.Udon;
using VRC.Udon.Common.Interfaces;

// The second machine: its processor in Udon (experiments/udon_aot). The kernel was translated to
// Udon at build time (`cpu`, UdonAotProgramAsset); the programs it loads are interpreted there.
// This behaviour boots it from the world's images, runs its slices and is its console.
[UdonBehaviourSyncMode(BehaviourSyncMode.None)]
public class EmuUdonCpu : UdonSharpBehaviour
{
    public UdonBehaviour cpu;
    public EmuMachine machine;            // the shader's machine: one of the two runs at a time
    public EmuTerminal terminal;
    public EmuKeyboard consoleKeyboard;
    public Texture2D[] ramLanes, romLanes;
    public Material unpackMaterial;       // UdonAotUnpack.shader
    public RenderTexture unpackTarget;    // 2048 x 2048: four million words a readback
    public TextMeshProUGUI label, powerLabel;
    public int sliceBudget = 2000;        // loop heads and calls a slice
    public float frameMs = 8f;            // Udon time a frame
    public int textSum;                   // of the translated kernel's text: another image is refused
    public int textWords;
    // the display: its RAM gathered at 0x87000000 into a stage, packed as state texels, drawn by Display.shader
    public Texture2D stageSmall, stageLarge;   // 8192 words a row: 64 rows, 256 rows
    public Material packMaterial;               // UdonAotPack.shader
    public RenderTexture udonState;             // 2048 x 256 texels of uint4
    public Material displayMaterial;            // Display.shader, _StateRow -3584
    // the sound card: its words and each voice's samples gathered the same way, mixed by the world's EmuSound
    public Texture2D soundStage;                // 8192 x 64
    public RenderTexture soundState;            // 2048 x 64 texels
    public Material soundControl;               // Sound.shader: where EmuSound.Mix leaves its cursor
    // the GPU (docs/machine/gpu.md): the rows of 0x86000000 to 0x88000000 a list reads, drawn by the
    // world's GPU camera, the picture read back into RAM
    public RenderTexture gpuState;              // 2048 x 1024 texels
    public Material gpuPackMaterial;            // UdonAotPack, _Keep 1
    public Material gpuBackMaterial;            // UdonAotGpuBack.shader
    public RenderTexture gpuBack;               // the GPU target's size, RGBA32
    // fetches (docs/system/fetch.md): pages and files by VRChat's loaders from the machine's addresses,
    // pictures through UdonAotFetch and a readback, the kept folder from EmuPersist's PlayerData
    public Material fetchMaterial;              // UdonAotFetch.shader
    public RenderTexture fetchTarget;           // 256 x 256 RGBA32: a part's 65,536 words
    public bool selfTest;                       // for a test client: boots, types, draws and plays by itself, logging "[UdonCpuTest]"

    private const int RamWords = 0x12000000 / 4, ChunkWords = 2048 * 2048;
    private const int ConHost = 0x076b8280 / 4, ConGuest = 0x076b8290 / 4, ConRing = 0x076b82a0 / 4;
    private const int TextAt = 0x00400000 / 4;
    private const int Dev = 0x07000000 / 4;              // 0x87000000: the display's and the input's words
    private const int DevAddress = -2030043136;          // 0x87000000
    private const int StageRow = 8192;

    private int[] gathered;
    private byte[] bytesSmall, bytesLarge;
    private bool shown;
    private int keyCount;

    private int[] mem, rom, payload;
    private byte[] chunk;
    private bool powered, loaded, loading, wanted;
    private int part, chunkAt, readbacks;
    private long guestTime;
    private int conRead, slices, idleFrames, framesOn;
    private float shownAt;
    private double busyMs;
    [HideInInspector] public double cpuMs, displayMs, gpuMs, soundMs, backMs;   // a frame's parts, summed
    [HideInInspector] public double markMs, uploadMs, renderMs, uploadRows;
    private System.Diagnostics.Stopwatch watch, clock;
    private int ranBefore;
    private float ranAt, mips;

    public bool Powered() { return powered; }

    public void Power()
    {
        if (powered) PowerOff();
        else PowerOn();
    }

    public void PowerOn()
    {
        if (machine != null && machine.Powered()) machine.PowerOff();
        if (!loaded)
        {
            wanted = true;
            if (!loading) StartLoading();
            Show();
            return;
        }
        bootAt = Time.realtimeSinceStartup;
        prompts = 0;
        if (mem == null) mem = new int[RamWords];
        else System.Array.Clear(mem, 0, RamWords);
        System.Array.Copy(payload, mem, payload.Length);
        for (int r = 1; r < 32; r++) cpu.SetProgramVariable("x" + r, 0);
        cpu.SetProgramVariable("x11", -2111832064);      // 0x82200000, the device tree
        cpu.SetProgramVariable("priv", 1);
        cpu.SetProgramVariable("mode", 1);
        cpu.SetProgramVariable("res", -1);
        cpu.SetProgramVariable("rpc", -2143289344);      // 0x80400000
        cpu.SetProgramVariable("resume", cpu.GetProgramVariable("resume0"));
        string[] zero = { "th", "tl", "fcsr", "msrc", "mdst", "mn", "ipc", "icount", "gcount" };
        foreach (string z in zero) cpu.SetProgramVariable(z, 0);
        cpu.SetProgramVariable("mem", mem);
        cpu.SetProgramVariable("rom", rom);
        cpu.SetProgramVariable("romlen", rom.Length);
        cpu.SetProgramVariable("xr", new int[32]);
        cpu.SetProgramVariable("fr", new int[32]);
        cpu.SetProgramVariable("csr", new int[16]);
        gpuBusy = false; gpuLists = 0; pendingCount = 0; fetchState = 0; heldAddress = "";
        guestTime = 1; keyCount = 0; ranBefore = 0; ranAt = 0f; mips = 0f;
        if (clock == null) clock = new System.Diagnostics.Stopwatch();
        clock.Restart(); conRead = 0; slices = 0; busyMs = 0; framesOn = 0; idleFrames = 0;
        if (terminal != null) terminal.Clear();
        if (consoleKeyboard != null) consoleKeyboard.Flush();
        powered = true;
        Show();
    }

    public void PowerOff()
    {
        powered = false;
        wanted = false;
        Blank();
        Show();
    }

    // ---- the boot images, a readback of four million words at a time

    private void StartLoading()
    {
        loading = true;
        chunk = new byte[ChunkWords * 4];
        payload = new int[ramLanes[0].width * ramLanes[0].height * 4];
        rom = new int[romLanes[0].width * romLanes[0].height * 4];
        part = 0; chunkAt = 0; readbacks = 0;
        Unpack();
    }

    private void Unpack()
    {
        Texture2D[] lanes = part == 0 ? ramLanes : romLanes;
        unpackMaterial.SetTexture("_LaneR", lanes[0]);
        unpackMaterial.SetTexture("_LaneG", lanes[1]);
        unpackMaterial.SetTexture("_LaneB", lanes[2]);
        unpackMaterial.SetTexture("_LaneA", lanes[3]);
        unpackMaterial.SetFloat("_Texel0", chunkAt / 4);
        VRCGraphics.Blit(lanes[0], unpackTarget, unpackMaterial);
        VRCAsyncGPUReadback.Request(unpackTarget, 0, TextureFormat.RGBA32, (IUdonEventReceiver)this);
    }

    public override void OnAsyncGpuReadbackComplete(VRCAsyncGPUReadbackRequest request)
    {
        if (!loading && pendingCount > 0)
        {
            // requests complete in the order made: whose this is
            int kind = pending[pendingHead];
            pendingHead = (pendingHead + 1) & 7;
            pendingCount--;
            if (kind == 1)
            {
                if (watch == null) watch = new System.Diagnostics.Stopwatch();
                double t = watch.Elapsed.TotalMilliseconds;
                GpuBack(request);
                backMs += watch.Elapsed.TotalMilliseconds - t;
            }
            else FetchBack(request);
            return;
        }
        if (!loading) return;
        if (request.hasError || !request.TryGetData(chunk))
        {
            Unpack();     // ask again
            return;
        }
        readbacks++;
        int[] into = part == 0 ? payload : rom;
        int n = Mathf.Min(ChunkWords, into.Length - chunkAt);
        System.Buffer.BlockCopy(chunk, 0, into, chunkAt * 4, n * 4);
        chunkAt += n;
        if (chunkAt >= into.Length)
        {
            part++; chunkAt = 0;
            if (part == 2)
            {
                loading = false;
                chunk = null;
                loaded = Sum() == textSum;
                if (!loaded) { wanted = false; Show(); return; }
                if (wanted) PowerOn();
                return;
            }
        }
        Unpack();
        Show();
    }

    // every 61st word of the kernel's text, as the editor summed the image it translated
    private int Sum()
    {
        int s = 0;
        for (int i = 0; i < textWords; i += 61) s = s * 31 + payload[TextAt + i];
        return s;
    }

    // ---- running

    private void Update()
    {
        if (selfTest) TestStep();
        if (!powered) return;
        if (machine != null && machine.Powered()) { PowerOff(); return; }
        if (!cpu.enabled) { powered = false; label.text = "Udon CPU: stopped (Udon halted the program)"; return; }
        if (watch == null) watch = new System.Diagnostics.Stopwatch();
        watch.Restart();
        Input();
        Rtc();
        // the guest's timer (5000 Hz, docs: the device tree's timebase) is real time since power on
        if (clock == null) clock = new System.Diagnostics.Stopwatch();
        bool idle = false;
        while (watch.Elapsed.TotalMilliseconds < frameMs)
        {
            long real = (long)(clock.Elapsed.TotalMilliseconds * 5.0);
            if (real > guestTime) guestTime = real;
            cpu.SetProgramVariable("now", (int)guestTime);
            cpu.SetProgramVariable("budget", sliceBudget);
            cpu.SetProgramVariable("status", 0);
            cpu.SendCustomEvent("Slice");
            int status = (int)cpu.GetProgramVariable("status");
            slices++;
            Printed();
            if (status == 2)
            {
                // waiting for an interrupt: the rest of the frame is free unless the timer is due already
                long tcmp = ((long)(uint)(int)cpu.GetProgramVariable("th") << 32) | (uint)(int)cpu.GetProgramVariable("tl");
                if (tcmp <= (long)(clock.Elapsed.TotalMilliseconds * 5.0)) continue;
                idle = true;
                break;
            }
        }
        double t0 = watch.Elapsed.TotalMilliseconds;
        Display();
        double t1 = watch.Elapsed.TotalMilliseconds;
        Gpu();
        double t2 = watch.Elapsed.TotalMilliseconds;
        Sound();
        cpuMs += t0; displayMs += t1 - t0; gpuMs += t2 - t1; soundMs += watch.Elapsed.TotalMilliseconds - t2;
        busyMs += watch.Elapsed.TotalMilliseconds;
        framesOn++;
        if (idle) idleFrames++;
        if (Time.time - shownAt > 1f) Show();
    }

    // ---- the display (docs/machine/display.md) and the input words (input.md)

    private void Display()
    {
        int mode = mem[Dev], w = mem[Dev + 1], h = mem[Dev + 2];
        if (mode < 1 || mode > 4 || w <= 0 || h <= 0 || w > 2048 || h > 2048) { Blank(); return; }
        if (gathered == null)
        {
            gathered = new int[StageRow * stageLarge.height];
            bytesSmall = new byte[StageRow * stageSmall.height * 4];
            bytesLarge = new byte[gathered.Length * 4];
        }
        int cap = gathered.Length;
        System.Array.Copy(mem, Dev, gathered, 0, 1024);
        int n = 1024;
        if (mode == 1) n = Gather(Dev + 1024, w * h, n, cap, gathered);
        else if (mode == 2) n = Gather(Dev + 1024, (w * h + 3) / 4, n, cap, gathered);
        else
        {
            // the layer table: a count, then x, y, width, height and address, 8 words a layer
            int t = Index(mem[Dev + 3]);
            if (t + 132 > mem.Length) { Blank(); return; }
            int count = Mathf.Clamp(mem[t], 0, 16);
            int tn = n;
            System.Array.Copy(mem, t, gathered, tn, 4 + 8 * count);
            gathered[3] = DevAddress + tn * 4;
            n = (tn + 4 + 8 * count + 3) & ~3;
            for (int k = 1; k <= count; k++)
            {
                int size = gathered[tn + 8 * k - 2] * gathered[tn + 8 * k - 1];
                int at = Gather(Index(gathered[tn + 8 * k]), size, n, cap, gathered);
                if (at == n) gathered[tn + 8 * k - 2] = 0;       // not gathered: shown as no layer
                else gathered[tn + 8 * k] = DevAddress + n * 4;
                n = (at + 3) & ~3;
            }
        }
        if ((gathered[18] & 1) != 0)
        {
            int at = Gather(Index(gathered[19]), 1024, n, cap, gathered);
            if (at == n) gathered[18] = 0;
            else gathered[19] = DevAddress + n * 4;
            n = at;
        }
        Texture2D stage = n <= bytesSmall.Length / 4 ? stageSmall : stageLarge;
        byte[] bytes = stage == stageSmall ? bytesSmall : bytesLarge;
        System.Buffer.BlockCopy(gathered, 0, bytes, 0, bytes.Length);
        stage.LoadRawTextureData(bytes);
        stage.Apply(false);
        packMaterial.SetFloat("_Rows", stage.height);
        VRCGraphics.Blit(stage, udonState, packMaterial);
        displayMaterial.SetTexture("_State", udonState);
        displayMaterial.SetTexture("_GpuTarget", machine.gpuCamera.targetTexture);
        displayMaterial.SetFloat("_Power", 1f);
        VRCGraphics.Blit(udonState, machine.displayTexture, displayMaterial);
        machine.shownWidth = w;
        machine.shownHeight = h;
        machine.displayShowMaterial.SetVector("_Size", new Vector4(w, h, 0, 0));
        shown = true;
    }

    private const int RamBase = -2147483648;   // 0x80000000, mem[0]

    private int Index(int address) { return (address & 0x7fffffff) >> 2; }

    // `words` words of guest RAM from mem[src] into into[n..]: where they end, or n if they do not fit
    private int Gather(int src, int words, int n, int cap, int[] into)
    {
        if (words <= 0 || n + words > cap || src + words > mem.Length) return n;
        System.Array.Copy(mem, src, into, n, words);
        return n + words;
    }

    private void Blank()
    {
        if (!shown || machine == null) return;
        shown = false;
        if (machine.Powered()) return;
        VRCGraphics.Blit(Texture2D.blackTexture, machine.displayTexture);
        machine.shownWidth = 0;
        machine.shownHeight = 0;
        machine.displayShowMaterial.SetVector("_Size", Vector4.zero);
    }

    // ---- the GPU: a submitted list's rows uploaded, drawn by the shader machine's GPU camera, and
    // the copy it asks for read back; then the submit word is taken back and the counters move.

    private const int GpuLo = -2046820352;     // 0x86000000
    private const int GpuRows = 1024, RowWords = 8192;
    private int[] rowStamp, backWords;
    private byte[] backBytes;
    private int stamp, gpuLists, copyAt, copyW, copyH, copyRow;
    private bool gpuBusy;

    private void Gpu()
    {
        int submit = mem[Dev + 4];
        if (gpuBusy || (submit & 1) == 0 || gathered == null) return;
        if (rowStamp == null) rowStamp = new int[GpuRows];
        stamp++;
        double g0 = watch.Elapsed.TotalMilliseconds;
        int list = mem[Dev + 5], count = Mathf.Clamp(mem[Dev + 6], 0, 4096);
        Mark(DevAddress, 0x8000);
        Mark(list, count * 64);
        if (Inside(list))
            for (int c = 0; c < count; c++) Command(Index(list) + 16 * c);
        double g1 = watch.Elapsed.TotalMilliseconds;
        Upload();
        double g2 = watch.Elapsed.TotalMilliseconds;
        markMs += g1 - g0; uploadMs += g2 - g1;
        Material m = machine.gpuMaterial;
        m.SetTexture("_State", gpuState);
        m.SetFloat("_StateRow", -(0x6000000 / 16 / 2048));
        machine.gpuCamera.Render();
        renderMs += watch.Elapsed.TotalMilliseconds - g2;
        m.SetFloat("_StateRow", 64);
        gpuLists++;
        if ((submit & 6) == 0) { Drawn(false); return; }
        if ((submit & 4) != 0)
        {
            copyAt = mem[Dev + 20]; copyW = mem[Dev + 21]; copyH = mem[Dev + 22]; copyRow = Mathf.Max(mem[Dev + 23], copyW);
        }
        else
        {
            copyAt = DevAddress + 0x1000; copyW = mem[Dev + 1]; copyH = mem[Dev + 2]; copyRow = copyW;
        }
        VRCGraphics.Blit(machine.gpuCamera.targetTexture, gpuBack, gpuBackMaterial);
        Readback(gpuBack, 1);
        gpuBusy = true;
    }

    private int[] pending = new int[8];
    private int pendingHead, pendingCount;

    private void Readback(RenderTexture rt, int kind)
    {
        pending[(pendingHead + pendingCount) & 7] = kind;
        pendingCount++;
        VRCAsyncGPUReadback.Request(rt, 0, TextureFormat.RGBA32, (IUdonEventReceiver)this);
    }

    private void GpuBack(VRCAsyncGPUReadbackRequest request)
    {
        int w = gpuBack.width, h = gpuBack.height;
        if (backBytes == null) { backBytes = new byte[w * h * 4]; backWords = new int[w * h]; }
        if (request.hasError || !request.TryGetData(backBytes))
        {
            Readback(gpuBack, 1);
            return;
        }
        System.Buffer.BlockCopy(backBytes, 0, backWords, 0, backBytes.Length);
        // only into what a GPU program may own, 0x86000000 to 0x87e00000
        int first = Index(copyAt), cw = Mathf.Min(copyW, w), ch = Mathf.Min(copyH, h);
        int lo = Index(GpuLo), hi = lo + 0x1e00000 / 4;
        for (int y = 0; y < ch && cw > 0; y++)
        {
            int at = first + y * copyRow;
            if (at < lo || at + cw > hi) break;
            System.Array.Copy(backWords, y * w, mem, at, cw);
        }
        Drawn(true);
    }

    private void Drawn(bool copied)
    {
        mem[Dev + 4] = 0;
        mem[Dev + 7]++;
        if (copied) mem[Dev + 14]++;
        gpuBusy = false;
    }

    private bool Inside(int address) { int off = address - GpuLo; return off >= 0 && off < 0x2000000; }

    // the rows of 0x86000000 to 0x88000000 that bytes at address cover are wanted
    private void Mark(int address, int bytes)
    {
        int off = address - GpuLo;
        if (bytes <= 0 || off < 0 || off >= 0x2000000) return;
        int end = Mathf.Min(off + Mathf.Min(bytes, 0x2000000) - 1, 0x1ffffff);
        for (int r = off >> 15; r <= end >> 15; r++) rowStamp[r] = stamp;
    }

    // what command c reads, by gpu.md's table (glyphs of distance fields aside)
    private void Command(int c)
    {
        int op = mem[c];
        if (op == 2) Texture(mem[c + 8], mem[c + 9], mem[c + 10], mem[c + 11]);
        else if (op == 3)
        {
            int n = Mathf.Clamp(mem[c + 2], 0, 196608), vm = mem[c + 4], fm = mem[c + 5];
            Mark(mem[c + 1], n * ((vm & 0x4000) != 0 ? 4 : (vm & 0x400) != 0 ? 16 : 64));
            if ((vm & 0x4000) != 0) Mark(mem[c + 12], n * 4);
            Mark(mem[c + 6], 256);
            Texture(fm, mem[c + 7], mem[c + 8], mem[c + 9]);
            if ((vm & 0x2000) != 0) Mark(mem[c + 11], 1024);
            if ((vm & 0x20000) != 0) { Mark(mem[c + 11], 4096); Mark(mem[c + 13], 48); }
            if ((vm & 0x10000) != 0) Mark(mem[c + 15], 32);
            if ((fm & 0x800) != 0)
            {
                int size = mem[c + 14];
                Mark(mem[c + 12], n * 4);
                Mark(mem[c + 13], (size & 0xffff) * ((size >> 16) & 0xffff) * 3);
            }
        }
        else if (op == 4)
        {
            int chars = Mathf.Clamp(mem[c + 6], 0, 4096), str = mem[c + 7], table = mem[c + 2], fw = mem[c + 8];
            Mark(str, chars * 4);
            if (!Inside(str) || !Inside(table)) return;
            int from = (fw & 0x100) != 0 ? mem[c + 11] : 0, s = Index(str);
            for (int k = 0; k < chars; k++)
            {
                int t = table + 16 * (mem[s + k] & 0xffff);
                Mark(t, 16);
                if (!Inside(t) || (fw & 0xff) == 8) continue;
                int e = Index(t);
                Mark(from + mem[e], ((mem[e + 1] + 1) >> 1) * mem[e + 2]);
            }
        }
    }

    private void Texture(int fm, int address, int w, int h)
    {
        int k = fm & 0xff;
        w = Mathf.Clamp(w, 0, 4096); h = Mathf.Clamp(h, 0, 4096);
        if (k == 1) Mark(address, 4 * w * h);
        else if (k == 2) Mark(address, w * h);
        else if (k == 3) Mark(address, ((w + 7) >> 3) * h);
        else if (k == 4) Mark(address, 3 * w * h);
        else if (k == 6) Mark(address, w * h * 3 / 2);
        else if (k == 5)
        {
            Mark(address, 16);
            if (!Inside(address)) return;
            int t = Index(address), d = mem[t + 3], tw = Mathf.Max(d & 0xff, 1), th = Mathf.Max((d >> 8) & 0xff, 1);
            Mark(mem[t], ((d >> 16) & 0xfff) * (h / th + 1) * 2);
            Mark(mem[t + 1], 1024 * tw * th);
            Mark(mem[t + 2], 1024);
        }
    }

    // the wanted rows into gpuState, up to the small stage's 64 rows a blit
    private void Upload()
    {
        int lo = Index(GpuLo), most = stageSmall.height, r = 0;
        while (r < GpuRows)
        {
            if (rowStamp[r] != stamp) { r++; continue; }
            int r0 = r;
            while (r < GpuRows && rowStamp[r] == stamp && r - r0 < most) r++;
            int words = (r - r0) * RowWords;
            uploadRows += r - r0;
            System.Array.Copy(mem, lo + r0 * RowWords, gathered, 0, words);
            System.Buffer.BlockCopy(gathered, 0, bytesSmall, 0, words * 4);
            stageSmall.LoadRawTextureData(bytesSmall);
            stageSmall.Apply(false);
            gpuPackMaterial.SetFloat("_Rows", r - r0);
            gpuPackMaterial.SetFloat("_DstRow", r0);
            VRCGraphics.Blit(stageSmall, gpuState, gpuPackMaterial);
        }
    }

    // ---- fetches: the words at 0x87000100 (requests, length, kind, offset, answered, bytes,
    // status, a picture's size, the address), the answer's 256 KB at 0x876c0000

    private const int FetchData = 0x076c0000 / 4, FetchMost = 262144, KindFile = 3;
    private const string BlobKey = "persist.blob";
    private int fetchState;                 // 0 idle, 1 loading, 2 reading a picture's part back
    private int fetchSeq, fetchKind, fetchOffset, fetchLength, fetchW, fetchH;
    private float fetchStarted;
    private string fetchAddress = "", heldAddress = "", persistWhat = "";
    private byte[] heldBytes, fetchPart;
    private Texture2D heldImage;
    private bool heldIsImage, packedChecked, packed;
    private VRCImageDownloader imageLoader;
    public string fetchSays = "";           // what the last fetch came to, for the status label

    private void Fetch()
    {
        if (fetchState == 1 && persistWhat.Length > 0) { PersistAnswer(); return; }
        if (fetchState != 0 || mem[Dev + 64] == mem[Dev + 68]) return;
        fetchSeq = mem[Dev + 64];
        int length = Mathf.Clamp(mem[Dev + 65], 0, 255);
        string address = "";
        for (int i = 0; i < length; i++) address += (char)((mem[Dev + 72 + i / 4] >> (8 * (i % 4))) & 0xff);
        fetchKind = mem[Dev + 66];
        fetchOffset = Mathf.Clamp(mem[Dev + 67], 0, 0xffffff);
        fetchAddress = address;
        if (fetchOffset != 0 && address == heldAddress && address.Length > 0)
        {
            Deliver();     // a later part of an answer that is kept
            return;
        }
        heldAddress = ""; heldBytes = null; heldImage = null;
        if (imageLoader != null) { imageLoader.Dispose(); imageLoader = null; }
        if (address == "host:name")
        {
            VRCPlayerApi me = Networking.LocalPlayer;
            string name = Utilities.IsValid(me) ? me.displayName : "player";
            byte[] b = new byte[Mathf.Min(name.Length, 64)];
            for (int i = 0; i < b.Length; i++) b[i] = name[i] > ' ' && name[i] < 127 ? (byte)name[i] : (byte)'_';
            Text(b, 200);
            return;
        }
        if (address == "power:off") { Answer(0, 2); PowerOff(); return; }
        if (address.StartsWith("persist:"))
        {
            persistWhat = address.Substring(8);
            fetchState = 1;
            fetchStarted = Time.time;
            PersistAnswer();
            return;
        }
        VRCUrl url = UrlOf(address);
        if (!address.StartsWith("https://") && address != "world:typed") { Answer(0, 3); fetchSays = "VRChat opens only https addresses"; return; }
        if (url == null || !url.Get().StartsWith("https://")) { Answer(0, 1); fetchSays = "Not handed over: " + address; return; }
        heldAddress = address;
        heldIsImage = fetchKind == 1 || (fetchKind == KindFile && IsPicture(address));
        fetchState = 1;
        fetchStarted = Time.time;
        fetchSays = "Loading " + address;
        if (heldIsImage)
        {
            imageLoader = new VRCImageDownloader();
            TextureInfo info = new TextureInfo();
            info.GenerateMipMaps = false;
            imageLoader.DownloadImage(url, null, (IUdonEventReceiver)this, info);
        }
        else VRCStringDownloader.LoadUrl(url, (IUdonEventReceiver)this);
    }

    // an address a visitor handed over in one of the machine's fields, or one the world was built with
    private VRCUrl UrlOf(string address)
    {
        VRCUrl url = null;
        for (int i = 0; i < machine.typedUrls.Length; i++)
            if (machine.typedUrls[i].GetUrl().Get() == address || (i == 0 && address == "world:typed")) url = machine.typedUrls[i].GetUrl();
        if (machine.openField != null && machine.openField.GetUrl().Get() == address) url = machine.openField.GetUrl();
        if (machine.consoleField != null && machine.consoleField.GetUrl().Get() == address) url = machine.consoleField.GetUrl();
        int low = 0, high = machine.siteAddresses.Length;
        while (low < high && url == null)
        {
            int middle = (low + high) / 2;
            int order = string.CompareOrdinal(address, machine.siteAddresses[middle]);
            if (order == 0) url = machine.siteUrls[middle];
            else if (order < 0) high = middle;
            else low = middle + 1;
        }
        return url;
    }

    private bool IsPicture(string address)
    {
        int cut = address.IndexOf('?');
        string path = (cut < 0 ? address : address.Substring(0, cut)).ToLower();
        return path.EndsWith(".png") || path.EndsWith(".jpg") || path.EndsWith(".jpeg");
    }

    // the answer's words; its bytes are in place already
    private void Answer(int length, int status)
    {
        mem[Dev + 69] = length;
        mem[Dev + 70] = status;
        mem[Dev + 68] = fetchSeq;
        fetchState = 0;
    }

    private void Text(byte[] bytes, int status)
    {
        heldBytes = bytes;
        heldIsImage = false;
        fetchOffset = 0;
        fetchLength = bytes.Length;
        int n = Mathf.Min(bytes.Length, FetchMost);
        System.Buffer.BlockCopy(bytes, 0, mem, FetchData * 4, n);
        Answer(bytes.Length, status);
    }

    // the part at fetchOffset of what is held
    private void Deliver()
    {
        if (!heldIsImage)
        {
            if (heldBytes == null) { Answer(0, 2); return; }
            int at = Mathf.Min(fetchOffset, heldBytes.Length) & ~3, n = Mathf.Min(heldBytes.Length - at, FetchMost);
            if (n > 0) System.Buffer.BlockCopy(heldBytes, at, mem, FetchData * 4, n);
            Answer(heldBytes.Length, 200);
            return;
        }
        if (heldImage == null) { Answer(0, 2); return; }
        fetchMaterial.SetFloat("_Offset", packedChecked ? fetchOffset : 0);
        fetchMaterial.SetFloat("_Packed", fetchKind == KindFile && (packed || !packedChecked) ? 1 : 0);
        VRCGraphics.Blit(heldImage, fetchTarget, fetchMaterial);
        Readback(fetchTarget, 2);
        fetchState = 2;
    }

    private void FetchBack(VRCAsyncGPUReadbackRequest request)
    {
        if (fetchState != 2) return;
        if (fetchPart == null) fetchPart = new byte[FetchMost];
        if (request.hasError || !request.TryGetData(fetchPart)) { Deliver(); return; }
        if (!packedChecked)
        {
            packedChecked = true;
            // a file in the picture says so in its first twelve bytes (docs/system/fetch.md)
            packed = fetchKind == KindFile && fetchPart[0] == 'S' && fetchPart[1] == 'H' && fetchPart[2] == 'A' && fetchPart[3] == 'D'
                     && fetchPart[8] == 'U' && fetchPart[9] == 0x1a && fetchPart[10] == 'F' && fetchPart[11] == '1';
            if (packed) fetchLength = 256 + (fetchPart[12] | fetchPart[13] << 8 | fetchPart[14] << 16 | fetchPart[15] << 24);
            if (fetchOffset != 0 || (fetchKind == KindFile && !packed)) { Deliver(); return; }
        }
        System.Buffer.BlockCopy(fetchPart, 0, mem, FetchData * 4, FetchMost);
        if (packed) Answer(fetchLength, 201);
        else
        {
            mem[Dev + 71] = fetchW | fetchH << 16;
            Answer(fetchW * fetchH * 4, 200);
        }
    }

    public override void OnImageLoadSuccess(IVRCImageDownload result)
    {
        if (fetchState != 1 || persistWhat.Length > 0) return;
        heldImage = result.Result;
        fetchW = heldImage.width; fetchH = heldImage.height;
        packedChecked = fetchKind != KindFile; packed = false;
        fetchSays = "Loaded a picture, " + fetchW + " x " + fetchH;
        Deliver();
    }

    public override void OnImageLoadError(IVRCImageDownload result)
    {
        if (fetchState != 1) return;
        int status = 415, code;
        string message = result.ErrorMessage;
        int at = message == null ? -1 : message.IndexOf("HTTP/1.1 ");
        if (at >= 0 && message.Length >= at + 12 && int.TryParse(message.Substring(at + 9, 3), out code) && code >= 400) status = code;
        heldAddress = "";
        fetchSays = "Picture not loaded: " + message;
        Answer(0, status);
    }

    public override void OnStringLoadSuccess(IVRCStringDownload result)
    {
        if (fetchState != 1 || persistWhat.Length > 0) return;
        heldBytes = result.ResultBytes;
        heldIsImage = false;
        fetchSays = "Loaded " + heldBytes.Length.ToString("N0") + " bytes";
        Deliver();
    }

    public override void OnStringLoadError(IVRCStringDownload result)
    {
        if (fetchState != 1) return;
        heldAddress = "";
        fetchSays = "Not loaded: " + result.Error;
        Answer(0, result.ErrorCode > 1 && result.ErrorCode != 200 ? result.ErrorCode : 2);
    }

    // persist:load, load!, save, clear, auto=0, auto=1, state (docs/system/persist.md), once VRChat
    // has brought this player's data; the folder is EmuPersist's, the same for both machines
    private void PersistAnswer()
    {
        EmuPersist kept = machine.persist;
        if (kept == null) { persistWhat = ""; Answer(0, 404); return; }
        if (!kept.Ready())
        {
            if (Time.time - fetchStarted > 30f) { persistWhat = ""; Answer(0, 404); }
            return;
        }
        string what = persistWhat;
        persistWhat = "";
        fetchSays = "The kept folder: " + what;
        if (what == "load" || what == "load!")
        {
            byte[] blob = kept.Blob();
            if (what == "load" && !kept.Auto()) Answer(0, 403);
            else if (blob.Length == 0 || blob.Length > FetchMost) Answer(0, 404);
            else Text(blob, 200);
        }
        else if (what == "save")
        {
            int bytes = fetchOffset;
            if (bytes > EmuPersist.Limit) { Answer(0, 413); return; }
            byte[] blob = new byte[Mathf.Max(bytes, 0)];
            if (bytes >= 16) System.Buffer.BlockCopy(mem, FetchData * 4, blob, 0, bytes);
            int length = bytes >= 16 ? blob[8] | blob[9] << 8 | blob[10] << 16 | blob[11] << 24 : -1;
            // its own mark and length first (nxpersist.c): anything else is not a folder
            if (bytes < 16 || blob[0] != 'S' || blob[1] != 'E' || blob[2] != 'P' || blob[3] != 'D' || length != bytes - 16) { Answer(0, 500); return; }
            PlayerData.SetBytes(BlobKey, blob);
            fetchSays = "The kept folder: saved " + bytes + " bytes";
            Answer(0, 200);
        }
        else if (what == "clear") { kept.Forget(); Answer(0, 200); }
        else if (what == "auto=0" || what == "auto=1") { kept.SetAuto(what == "auto=1"); Answer(0, 200); }
        else if (what == "state")
        {
            string text = "stored " + kept.Blob().Length + "\nlimit " + EmuPersist.Limit + "\nauto " + (kept.Auto() ? 1 : 0) + "\n";
            byte[] b = new byte[text.Length];
            for (int i = 0; i < b.Length; i++) b[i] = (byte)text[i];
            Text(b, 200);
        }
        else Answer(0, 400);
    }

    // ---- the sound card (docs/machine/sound.md; the shader's sound.h)

    private const int SndGuest = Dev + 0x88, SndDevice = Dev + 0x8c, SndVoices = 0x200, SndState = 0x300;
    private int[] soundGathered;
    private byte[] soundBytes;

    private void Sound()
    {
        EmuSound card = machine.sound;
        if (card == null) return;
        bool enabled = mem[SndGuest] != 0;
        int clock = mem[SndDevice];
        int count = Mathf.Clamp(mem[SndGuest + 2], 0, 32);
        if (enabled)
        {
            if (soundGathered == null)
            {
                soundGathered = new int[StageRow * soundStage.height];
                soundBytes = new byte[soundGathered.Length * 4];
            }
            System.Array.Copy(mem, Dev, soundGathered, 0, 1024);
            int n = 1024;
            for (int v = 0; v < count; v++)
            {
                int a = SndVoices + 8 * v, s = SndState + 8 * v;
                n = GatherVoice(a + 2, a + 3, a + 7, soundGathered[a + 1], n);
                n = GatherVoice(s + 4, s + 5, s + 7, soundGathered[s + 1] >> 8, n);
            }
            System.Buffer.BlockCopy(soundGathered, 0, soundBytes, 0, soundBytes.Length);
            soundStage.LoadRawTextureData(soundBytes);
            soundStage.Apply(false);
            packMaterial.SetFloat("_Rows", soundStage.height);
            VRCGraphics.Blit(soundStage, soundState, packMaterial);
            card.mixMaterial.SetFloat("_StateRow", -(0x700000 / 2048));
        }
        bool mixed = card.Mix(soundState, soundControl, enabled, (uint)(clock & 0x7fffffff));
        card.mixMaterial.SetFloat("_StateRow", 64);
        if (!mixed) return;
        // the control pass's part: every voice moved up to the cursor the mix began at
        int cursor = (soundControl.GetInt("_SoundCursorLo") & 0xffff) | (soundControl.GetInt("_SoundCursorHi") << 16);
        for (int v = 0; v < count; v++) VoiceAt(v, cursor, clock);
        mem[SndDevice] = cursor;
        mem[SndDevice + 1] = card.Rate();
    }

    // A voice's samples (and an ADPCM voice's checkpoints, an FM voice's instruments) into the
    // gathered copy, its addresses there rewritten. ROM addresses are the ROM's textures'.
    private int GatherVoice(int at, int lengthAt, int auxAt, int kind, int n)
    {
        int address = soundGathered[at], length = soundGathered[lengthAt], k = kind & 0xff;
        int wide = (kind & 0x400) != 0 ? 2 : 1;
        int bytes = k == 1 ? length * wide : k == 2 || k == 3 ? 2 * length * wide : k == 4 ? length / 2 + 16 : k == 5 ? 65536 : 0;
        n = GatherBytes(at, address, bytes, n);
        if (k == 4) n = GatherBytes(auxAt, soundGathered[auxAt], 4 * (length / 32 + 2), n);
        if (k == 5) n = GatherBytes(auxAt, soundGathered[auxAt], 4096, n);
        return n;
    }

    private int GatherBytes(int at, int address, int bytes, int n)
    {
        if (((address >> 30) & 3) != 2 || bytes <= 0 || bytes > 4 << 20) return n;
        int src = Index(address);
        int end = Gather(src, (bytes + (address & 3) + 3) / 4 + 1, n, soundGathered.Length, soundGathered);
        if (end == n) return n;
        soundGathered[at] = DevAddress + n * 4 + (address & 3);
        return (end + 3) & ~3;
    }

    // sound.h's snd_voice_at and snd_move, unsigned arithmetic in ints
    private void VoiceAt(int v, int cursor, int clock)
    {
        int a = Dev + SndVoices + 8 * v, s = Dev + SndState + 8 * v;
        int key = mem[a], pitch = mem[a + 5], kind, address, length, restart, aux, pos, frac, moved = 0;
        bool on;
        if (mem[a] != mem[s])
        {
            on = (key & 1) != 0; kind = mem[a + 1]; address = mem[a + 2]; length = mem[a + 3];
            restart = mem[a + 4]; aux = mem[a + 7]; pos = 0; frac = 0;
        }
        else
        {
            on = (mem[s + 1] & 1) != 0; kind = (mem[s + 1] >> 8) & 0xffffff; address = mem[s + 4]; length = mem[s + 5];
            restart = mem[s + 6]; aux = mem[s + 7]; pos = mem[s + 2]; frac = mem[s + 3];
            moved = cursor - clock;
        }
        bool stream = (kind & 0xff) == 3;
        if (stream) restart = mem[a + 4];
        if (on)
        {
            int kl = moved & 0xffff, kh = (moved >> 16) & 0xffff, pl = pitch & 0xffff, ph = (pitch >> 16) & 0xffff;
            int low = kl * pl + frac;
            pos += ((low >> 16) & 0xffff) + kl * ph + kh * pl + ((kh * ph) << 16);
            frac = low & 0xffff;
            if (stream)
            {
                if (restart - pos <= 0) { pos = restart; frac = 0; }
            }
            else if ((pos ^ RamBase) >= (length ^ RamBase))
            {
                if ((kind & 0x100) != 0 && (restart ^ RamBase) < (length ^ RamBase))
                {
                    long over = ((long)pos - restart) & 0xffffffffL, span = ((long)length - restart) & 0xffffffffL;
                    pos = restart + (int)(over - over / span * span);    // (Udon has no Int64 remainder)
                }
                else on = false;
            }
        }
        mem[s] = key; mem[s + 1] = (kind << 8) | (on ? 1 : 0); mem[s + 2] = pos; mem[s + 3] = frac;
        mem[s + 4] = address; mem[s + 5] = length; mem[s + 6] = restart; mem[s + 7] = aux;
    }

    // The pointer every frame and up to four key events, as the shader machine's control pass writes them.
    private void Devices()
    {
        Fetch();
        mem[Dev + 13] = (int)(Time.realtimeSinceStartup * 1000f);     // the host's clock, in ms
        int w = mem[Dev + 1], h = mem[Dev + 2];
        if (mem[Dev] < 1 || w <= 0 || h <= 0) return;
        mem[Dev + 8] = Mathf.Clamp(machine.pointerX, 0, w - 1);
        mem[Dev + 9] = Mathf.Clamp(machine.pointerY, 0, h - 1);
        mem[Dev + 10] = machine.pointerButtons;
        EmuKeyboard keys = machine.gpuKeyboard;
        for (int i = 0; i < 4 && keys != null && keys.Count() > 0; i++)
        {
            int e = keys.Pop();
            mem[Dev + 32 + (keyCount & 31)] = (e & 65536) != 0 ? (e & 0xffff) | RamBase : e & 0xffff;
            keyCount++;
        }
        mem[Dev + 11] = keyCount;
    }

    private void Input()
    {
        Devices();
        if (consoleKeyboard == null || consoleKeyboard.Count() == 0) return;
        int delivered = mem[ConHost], taken = mem[ConGuest];
        if (taken != delivered) return;
        int group = 0, n = 0;
        while (n < 4 && consoleKeyboard.Count() > 0)
        {
            int c = consoleKeyboard.Pop() & 0xff;
            if (c == 0) continue;     // 0 means "no character" to the guest
            group |= c << (8 * n);
            n++;
        }
        if (n == 0) return;
        mem[ConHost + 1] = group;
        mem[ConHost] = delivered + n;
    }

    private void Printed()
    {
        int written = mem[ConGuest + 1];
        if (written - conRead > 256) conRead = written - 256;
        while (conRead != written)
        {
            int c = (mem[ConRing + ((conRead & 255) >> 2)] >> ((conRead & 3) * 8)) & 255;
            if (terminal != null) terminal.PutChar(c);
            lastFour = (lastFour << 8) | c;
            if (lastFour == 0x2f202320) prompts++;    // "/ # "
            conRead++;
        }
    }

    // ---- the self-test (docs: the world's test flags)

    private int lastFour, prompts, testStep, testPrompts, testCopies;
    private float bootAt, testAt;

    private void TestStep()
    {
        float now = Time.realtimeSinceStartup;
        if (testStep == 0 && Time.time > 5f)
        {
            Debug.Log("[UdonCpuTest] power on");
            PowerOn();
            testStep = 1; testAt = Time.realtimeSinceStartup;
        }
        else if (testStep == 1 && prompts > 0)
        {
            Debug.Log("[UdonCpuTest] prompt after " + (now - bootAt).ToString("F1") + " s of booting, " + (now - testAt).ToString("F1") + " s since power on; " + label.text);
            TypeLine("uname -a; dd if=/dev/urandom of=/dev/fb0 bs=4096 count=50");
            testStep = 2; testAt = now; testPrompts = prompts;
        }
        else if (testStep == 2 && (prompts > testPrompts || now - testAt > 120f))
        {
            Debug.Log("[UdonCpuTest] after " + (now - testAt).ToString("F1") + " s: display mode " + mem[Dev] + " " + mem[Dev + 1] + "x" + mem[Dev + 2]
                      + ", shown " + machine.shownWidth + "x" + machine.shownHeight + "; console: " + Tail());
            TypeLine("cat /bin/busybox > /dev/sound");
            testStep = 3; testAt = now; testPrompts = prompts;
        }
        else if (testStep == 3 && (prompts > testPrompts || now - testAt > 120f))
        {
            Debug.Log("[UdonCpuTest] sound after " + (now - testAt).ToString("F1") + " s: " + (machine.sound == null ? "no card" : machine.sound.mixes + " mixes, " + machine.sound.late + " late")
                      + "; " + label.text);
            TypeLine("nano-X -p & sleep 1; demo-hello &");
            testStep = 4; testAt = now;
        }
        else if (testStep == 4 && (Layers() >= 2 || now - testAt > 120f))
        {
            Debug.Log("[UdonCpuTest] window system after " + (now - testAt).ToString("F1") + " s: display mode " + mem[Dev] + " " + mem[Dev + 1] + "x" + mem[Dev + 2]
                      + ", " + Layers() + " layers, cursor " + mem[Dev + 18] + "; " + label.text);
            testStep = 5; testAt = now;
        }
        else if (testStep == 5 && now - testAt > 15f)
        {
            Debug.Log("[UdonCpuTest] 15 s later: the window's pixel at (60, 70) " + WindowPixel().ToString("x8") + "; GPU lists " + mem[Dev + 7] + ", copies " + mem[Dev + 14] + "; " + label.text);
            TypeLine("glxgears &");
            testStep = 6; testAt = now;
        }
        else if (testStep == 6 && now - testAt > 10f)
        {
            testCopies = mem[Dev + 14];
            testStep = 7; testAt = now;
        }
        else if (testStep == 7 && now - testAt > 10f)
        {
            Debug.Log("[UdonCpuTest] glxgears in a window: " + ((mem[Dev + 14] - testCopies) / (now - testAt)).ToString("F1") + " copies a second, " + Layers() + " layers; " + label.text + "; console: " + Tail());
            TypeLine("kill %1; date; sleep 5; date; nxpersist name; nxpersist state");
            testStep = 8; testAt = now;
            Debug.Log("[UdonCpuTest] host time " + System.DateTime.Now.ToString("HH:mm:ss"));
        }
        else if (testStep == 8 && now - testAt > 20f)
        {
            Debug.Log("[UdonCpuTest] clock and kept folder: " + Tail(8) + " host time " + System.DateTime.Now.ToString("HH:mm:ss"));
            TypeLine("open --dry https://creators.vrchat.com/; open --dry https://a.fsdn.com/sd/NewsletterSubscription.png?v=2");
            testStep = 9; testAt = now;
        }
        else if (testStep == 9 && now - testAt > 40f)
        {
            Debug.Log("[UdonCpuTest] fetch: " + fetchSays + "; console: " + Tail(6));
            testStep = 10;
        }
    }

    private int Layers()
    {
        if (mem[Dev] != 4) return 0;
        int t = Index(mem[Dev + 3]);
        return t + 4 <= mem.Length ? mem[t] : 0;
    }

    // the top layer's pixel at (60, 70): inside demo-hello's window, white
    private int WindowPixel()
    {
        int n = Layers();
        if (n < 2) return 0;
        int t = Index(mem[Dev + 3]);
        int at = Index(mem[t + 8 * n]) + 70 * mem[t + 8 * n - 2] + 60;
        return at < mem.Length ? mem[at] : 0;
    }

    private void TypeLine(string text)
    {
        if (consoleKeyboard != null) consoleKeyboard.TypeText(text + "\n");
    }

    private string Tail() { return Tail(4); }

    private string Tail(int lines)
    {
        int row = terminal.CursorRow();
        string o = "";
        for (int r = Mathf.Max(0, row - lines + 1); r <= row; r++) o += terminal.LineText(r).Trim() + " | ";
        return o;
    }

    // the clock chip (DS1742) as the harness sets it: local time in BCD, the century in the control byte
    private void Rtc()
    {
        System.DateTime t = System.DateTime.Now;
        int year = t.Year;
        cpu.SetProgramVariable("rtc0", Bcd(year / 100) | Bcd(t.Second) << 8 | Bcd(t.Minute) << 16 | Bcd(t.Hour) << 24);
        cpu.SetProgramVariable("rtc1", Bcd((int)t.DayOfWeek + 1) | Bcd(t.Day) << 8 | Bcd(t.Month) << 16 | Bcd(year % 100) << 24);
    }

    private int Bcd(int v) { return (v / 10) << 4 | (v % 10); }

    // instructions a second, translated and interpreted, over the time since the last
    private void Measure()
    {
        int ran = (int)cpu.GetProgramVariable("gcount") + (int)cpu.GetProgramVariable("icount");
        float now = Time.realtimeSinceStartup;
        if (ranAt > 0f && now > ranAt) mips = (ran - ranBefore) / (now - ranAt) / 1e6f;
        ranBefore = ran; ranAt = now;
    }

    private void Show()
    {
        if (powered) Measure();
        shownAt = Time.time;
        if (powerLabel != null) powerLabel.text = powered || wanted ? "Udon CPU off" : "Udon CPU on";
        if (label == null) return;
        if (loading) label.text = "Udon CPU: loading the images, " + readbacks + " of " + ((payload.Length + rom.Length + ChunkWords - 1) / ChunkWords);
        else if (!loaded && readbacks > 0) label.text = "Udon CPU: these images are not the kernel it was translated from";
        else if (!powered) label.text = "Udon CPU: off";
        else label.text = string.Format("Udon CPU: {0:F2} MIPS, {1:N0} slices, {2:F1} ms a frame, idle {3} frames", mips, slices, busyMs / Mathf.Max(1, framesOn), idleFrames);
    }
}
