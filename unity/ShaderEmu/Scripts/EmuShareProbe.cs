using UdonSharp;
using UnityEngine;
using VRC.SDK3.Rendering;
using VRC.SDKBase;
using VRC.Udon.Common.Interfaces;

// For the test with two real clients (docs/share.md, "Judged while it moves"): three times a
// second, 24 x 18 pixels of the screen, with the instance's clock: of the display of a client
// whose machine is on, and of the picture a client that watches one has of it. Each writes
// "[ShareSig]" lines to its own log; tools/share_clients.py holds the two logs together.
[UdonBehaviourSyncMode(BehaviourSyncMode.None)]
public class EmuShareProbe : UdonSharpBehaviour
{
    public EmuShareHub hub;
    public EmuMachine machine;
    public EmuStreams streams;
    public Material material;          // ShareUnits.shader, this behaviour's own: its pass "Signature"
    public RenderTexture signature;    // 24 x 18

    private const int Points = 24 * 18;
    private byte[] bytes = new byte[Points * 4];
    private float nextAt;
    private bool reading;
    private int role, stamp, units;

    void Update()
    {
        if (!hub.netTest || reading || Time.time < nextAt) return;
        nextAt = Time.time + 0.33f;
        Texture source;
        int w, h;
        if (machine.IsOn())
        {
            role = 1;
            source = machine.displayTexture;
            w = machine.displayWidth;
            h = machine.displayHeight;
        }
        else
        {
            int slot = hub.WallSlot();
            if (slot < 0) return;
            role = 2;
            source = streams.Picture(slot);
            w = streams.Width(slot);
            h = streams.Height(slot);
        }
        if (w <= 0 || h <= 0) return;
        material.SetVector("_TexSize", new Vector4(source.width, source.height, 0, 0));
        material.SetVector("_Src", new Vector4(w, h, 1, 0));
        VRCGraphics.Blit(source, signature, material, 12);
        stamp = Networking.GetServerTimeInMilliseconds();
        units = 1;
        VRCAsyncGPUReadback.Request(signature, 0, (IUdonEventReceiver)this);
        reading = true;
    }

    public override void OnAsyncGpuReadbackComplete(VRCAsyncGPUReadbackRequest request)
    {
        reading = false;
        if (request.hasError || !request.TryGetData(bytes)) return;
        string hex = "";
        for (int i = 0; i < Points; i++) hex += bytes[i * 4].ToString("X2") + bytes[i * 4 + 1].ToString("X2") + bytes[i * 4 + 2].ToString("X2");
        Debug.Log("[ShareSig] ms=" + stamp + " role=" + role + " units=" + units + " scene=" + hub.testScene + " " + hex);
    }
}
