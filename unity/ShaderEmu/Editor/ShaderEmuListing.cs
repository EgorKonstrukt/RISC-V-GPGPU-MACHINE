using System.IO;
using UnityEditor;
using UnityEngine;
using VRC.Core;
using VRC.SDKBase.Editor.Api;

// The world's entry at VRChat: its picture and its description, from the repository's
// unity/listing (tools/make_world_icon.py makes the picture). Neither needs a new build.
public static class ShaderEmuListing
{
    [MenuItem("ShaderEmu/Show the world's listing at VRChat")]
    public static async void Show()
    {
        PipelineManager pipeline = Object.FindObjectOfType<PipelineManager>();
        if (pipeline == null || string.IsNullOrEmpty(pipeline.blueprintId)) { Debug.LogError("[ShaderEmu] the scene has no world id"); return; }
        try
        {
            VRCWorld world = await VRCApi.GetWorld(pipeline.blueprintId, true);
            Debug.Log("[ShaderEmu] listing: \"" + world.Name + "\" (" + world.ReleaseStatus + "), capacity " + world.Capacity + "/" + world.RecommendedCapacity +
                      ", tags " + (world.Tags == null ? "" : string.Join(",", world.Tags)) + ", picture " + world.ImageUrl + "\n" + world.Description);
        }
        catch (System.Exception e) { Debug.LogError("[ShaderEmu] the listing was not read (is the SDK's panel logged in?): " + e.Message); }
    }

    [MenuItem("ShaderEmu/Update the world's description at VRChat")]
    public static void UpdateWords() { Update(false); }

    [MenuItem("ShaderEmu/Update the world's picture and description at VRChat")]
    public static void UpdateBoth() { Update(true); }

    static async void Update(bool withPicture)
    {
        string folder = ShaderEmuImages.Repo + "/unity/listing", picture = folder + "/WorldIcon.png", words = folder + "/description.txt";
        PipelineManager pipeline = Object.FindObjectOfType<PipelineManager>();
        if (pipeline == null || string.IsNullOrEmpty(pipeline.blueprintId)) { Debug.LogError("[ShaderEmu] the scene has no world id"); return; }
        if (!File.Exists(picture) || !File.Exists(words)) { Debug.LogError("[ShaderEmu] no " + picture + " or " + words); return; }
        try
        {
            // everything else of the entry (its name, tags, capacities) stays as it is
            VRCWorld world = await VRCApi.GetWorld(pipeline.blueprintId, true);
            world.Description = File.ReadAllText(words).Trim();
            world = await VRCApi.UpdateWorldInfo(pipeline.blueprintId, world);
            if (withPicture) world = await VRCApi.UpdateWorldImage(pipeline.blueprintId, world, picture);
            Debug.Log("[ShaderEmu] listing updated: \"" + world.Name + "\", picture " + world.ImageUrl + "\n" + world.Description);
        }
        catch (System.Exception e) { Debug.LogError("[ShaderEmu] the listing was not updated: " + e.Message); }
    }
}
