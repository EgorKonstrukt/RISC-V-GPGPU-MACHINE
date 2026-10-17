using TMPro;
using UdonSharp;
using UnityEngine;
using VRC.SDKBase;

// The network's panel on the right wall (docs/system/lan.md): every visitor's machine with its address
// and what it has sent, and this visitor's own machine's counts of packets.
[UdonBehaviourSyncMode(BehaviourSyncMode.None)]
public class EmuLan : UdonSharpBehaviour
{
    public EmuShareHub hub;
    public EmuMachine machine;
    public TextMeshProUGUI ownLabel;    // this machine's counts
    public TextMeshProUGUI listLabel;   // a line a machine
    public int rows = 14;

    private float nextAt;

    private string Address(int id) { return "10.0." + (id >> 8) + "." + (id & 255); }

    private string Line(int id, string name, bool on, int batches)
    {
        if (name.Length > 18) name = name.Substring(0, 18);
        return Address(id).PadRight(12) + name.PadRight(19) + (on ? "on " : "off") + batches.ToString().PadLeft(8);
    }

    void Update()
    {
        if (Time.time < nextAt) return;
        nextAt = Time.time + 1f;
        hub.Prune();
        EmuShare mine = hub.Mine();
        VRCPlayerApi me = Networking.LocalPlayer;
        if (mine == null || me == null) return;
        int id = hub.MyId();
        bool on = machine.Powered();
        if (ownLabel != null)
            ownLabel.text = Address(id) + (on ? "  on" : "  off: packets for it are dropped") + "\n"
                          + "sent " + machine.LanOut() + "   heard for it " + machine.LanMine() + ", for others " + machine.LanOthers()
                          + ", while off " + machine.LanOff() + "\n"
                          + "given to Linux " + machine.LanGiven() + " (taken " + machine.LanTaken() + ")   waiting out " + machine.NetWaiting()
                          + ", in " + machine.LanQueuedIn() + (machine.LanFull() > 0 ? "   dropped, full " + machine.LanFull() : "") + "\n"
                          + "sendings out " + hub.netBatches + "   packets in from others " + hub.netTaken;
        if (listLabel == null) return;
        string text = "ADDRESS     WHOSE              ON  SENDINGS\n" + Line(id, me.displayName, on, mine.netSeq);
        int others = hub.ShareCount();
        for (int i = 0; i < others && i < rows - 1; i++)
        {
            EmuShare share = hub.ShareAt(i);
            text += "\n" + Line(share.ownerId, share.ownerName, (share.flags & 1) != 0, share.netSeq);
        }
        if (others > rows - 1) text += "\nand " + (others - rows + 1) + " more";
        listLabel.text = text;
    }
}
