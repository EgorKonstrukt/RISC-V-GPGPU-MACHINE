using UdonSharp;
using UnityEngine;
using VRC.SDKBase;

// A holodeck's seat (docs/holodeck.md): a station of the scene, which VRChat syncs by itself,
// one visitor at a time. It says who sat down and who got up, and it rises under its sitter:
// VRChat puts them on a station standing and their pose sinks into sitting over half a second,
// so a seat at its height had them stand on it first. It waits on the floor and comes up as they sink.
[UdonBehaviourSyncMode(BehaviourSyncMode.None)]
public class EmuSeat : UdonSharpBehaviour
{
    public EmuHolodeck holodeck;
    public int room;
    public float height = 0.38f;   // where a sitter's hips are, above the floor the seat waits on
    public float settle = 0.5f;    // seconds VRChat takes from standing to sitting

    private bool taken;
    private float since;           // when the sitter came, or left

    public override void Interact()
    {
        VRCPlayerApi player = Networking.LocalPlayer;
        if (player != null) player.UseAttachedStation();
    }

    public override void OnStationEntered(VRCPlayerApi player)
    {
        taken = true;
        since = Time.time;
        if (Utilities.IsValid(player) && holodeck != null) holodeck.Entered(room, player);
    }

    public override void OnStationExited(VRCPlayerApi player)
    {
        taken = false;
        since = Time.time;
        if (Utilities.IsValid(player) && holodeck != null) holodeck.Exited(room, player);
    }

    void Update()
    {
        Vector3 at = transform.localPosition;
        float to = at.y;
        if (taken)
        {
            // (from wherever it is: a sitter put out and sat down again at once finds it up still)
            float t = settle > 0f ? Mathf.Clamp01((Time.time - since) / settle) : 1f;
            to = Mathf.Max(at.y, height * t * t * (3f - 2f * t));
        }
        else if (Time.time - since > 0.3f) to = 0f;
        if (to == at.y) return;
        at.y = to;
        transform.localPosition = at;
    }
}
