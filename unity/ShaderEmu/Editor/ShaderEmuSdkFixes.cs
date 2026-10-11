// What VRChat's build panel asks of a scene, done the room's way instead of by its Auto Fix
// buttons (those give a sound the SDK's own distances: a key's click heard 80 m off).
using System.Collections.Generic;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.UI;

public static partial class ShaderEmuBuilder
{
    [MenuItem("ShaderEmu/Fix what the SDK panel asks for in the open scene")]
    public static void SdkFixes()
    {
        int sounds = 0, graphics = 0, textures = 0;
        List<Renderer> renderers = new List<Renderer>();
        foreach (AudioSource source in Resources.FindObjectsOfTypeAll<AudioSource>())
        {
            if (!InScene(source)) continue;
            VRC.SDK3.Components.VRCSpatialAudioSource spatial = source.GetComponent<VRC.SDK3.Components.VRCSpatialAudioSource>();
            bool room = source.spatialBlend > 0f;
            // a source in the room keeps the distances its AudioSource was made with
            if (room && (spatial == null || spatial.Near != source.minDistance || spatial.Far != source.maxDistance || spatial.Gain != 0f))
            {
                Spatial(source, source.minDistance, source.maxDistance);
                sounds++;
            }
            else if (!room && spatial == null)
            {
                spatial = source.gameObject.AddComponent<VRC.SDK3.Components.VRCSpatialAudioSource>();
                spatial.EnableSpatialization = false;
                spatial.Gain = 0f;
                sounds++;
            }
        }
        foreach (Graphic graphic in Resources.FindObjectsOfTypeAll<Graphic>())
        {
            if (!InScene(graphic) || graphic is TMPro.TMP_SubMeshUI) continue;
            if (graphic.material == null || graphic.material.shader.name != "UI/Default") continue;
            if (graphic.materialForRendering == null || graphic.materialForRendering.shader.name != "UI/Default") continue;
            graphic.material = UiMaterial();
            EditorUtility.SetDirty(graphic);
            graphics++;
        }
        foreach (Renderer renderer in Resources.FindObjectsOfTypeAll<Renderer>()) if (InScene(renderer)) renderers.Add(renderer);
        // only textures on the room's renderers: a lightmap or a probe's picture is never among them
        foreach (TextureImporter importer in VRCSdkControlPanel.GetBoxFilteredTextureImporters(renderers))
        {
            importer.mipmapFilter = TextureImporterMipFilter.KaiserFilter;
            importer.SaveAndReimport();
            textures++;
        }
        if (sounds + graphics > 0) EditorSceneManager.MarkSceneDirty(EditorSceneManager.GetActiveScene());
        Debug.Log("[ShaderEmu] the SDK panel's alerts: " + sounds + " sounds, " + graphics + " UI graphics, " + textures + " textures set right (the scene is not saved)");
    }

    // The SDK's own test of what belongs to the scene (GatherComponentsOfTypeInScene).
    static bool InScene(Component thing)
    {
        return !EditorUtility.IsPersistent(thing.transform.root.gameObject)
            && thing.hideFlags != HideFlags.NotEditable && thing.hideFlags != HideFlags.HideAndDontSave;
    }
}
