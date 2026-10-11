# Puts the shader packages the room uses into the Unity project:
#   python world/install_packages.py C:/Development/VRChat/ShaderEmu
# LTCGI (pimaker: the screens light the room), VRC Light Volumes (RED_SIM: baked light for what
# moves), Mochie's shaders (its Standard takes both) and Silent's Fake Glass (the window's pane
# and its rain, from the room's reflection probes and no grab of the screen). Downloads are
# kept in build/packages.
import json
import os
import shutil
import sys
import tarfile
import urllib.request
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
CACHE = os.path.join(HERE, "..", "build", "packages")
LTCGI = ("at.pimaker.ltcgi", "1.7.3", "https://vpm.pimaker.at/downloads/at.pimaker.ltcgi_1.7.3.zip")
VOLUMES = ("red.sim.lightvolumes", "2.1.3", "https://github.com/REDSIM/VRCLightVolumes/releases/download/v.2.1.3/VRCLightVolumes_v.2.1.3.zip")
FAKE_GLASS = "https://gitlab.com/s-ilent/fake-glass/-/archive/master/fake-glass-master.zip"
MOCHIE = "https://github.com/MochiesCode/Mochies-Unity-Shaders/releases/download/v1.77.1/Mochies.Unity.Shaders.v1.77.1.Free.unitypackage"


def get(url, name):
    path = os.path.join(CACHE, name)
    if not os.path.exists(path):
        os.makedirs(CACHE, exist_ok=True)
        request = urllib.request.Request(url, headers={"User-Agent": "ShaderEmu-world-build"})
        with urllib.request.urlopen(request, timeout=300) as response, open(path, "wb") as out:
            shutil.copyfileobj(response, out)
    return path


def vpm(project, package):
    name, version, url = package
    target = os.path.join(project, "Packages", name)
    if os.path.exists(target):
        shutil.rmtree(target)
    with zipfile.ZipFile(get(url, name + ".zip")) as archive:
        archive.extractall(target)
    listing = os.path.join(project, "Packages", "vpm-manifest.json")
    manifest = json.load(open(listing, encoding="utf-8"))
    manifest["dependencies"][name] = {"version": version}
    manifest["locked"][name] = {"version": version, "dependencies": {}}
    json.dump(manifest, open(listing, "w", encoding="utf-8"), indent=2)
    print("installed", name, version)


def unity_package(project, url, skip=("Assets/csc.rsp",)):
    """A .unitypackage is a tar of folders, each an asset's path, contents and .meta."""
    count = 0
    with tarfile.open(get(url, "mochie.unitypackage")) as archive:
        members = {m.name: m for m in archive.getmembers()}
        for name, member in members.items():
            if not name.endswith("/pathname"):
                continue
            folder = name[:-len("pathname")]
            path = archive.extractfile(member).read().decode("utf-8").split("\n")[0].strip()
            if path in skip:
                continue
            target = os.path.join(project, path)
            asset, meta = members.get(folder + "asset"), members.get(folder + "asset.meta")
            if asset is None:
                os.makedirs(target, exist_ok=True)
            else:
                os.makedirs(os.path.dirname(target), exist_ok=True)
                open(target, "wb").write(archive.extractfile(asset).read())
                count += 1
            if meta is not None:
                open(target + ".meta", "wb").write(archive.extractfile(meta).read())
    print("installed Mochie's shaders:", count, "files")


def fake_glass(project):
    """Its Shader folder, metas and all, as its README says to place it."""
    target = os.path.join(project, "Assets", "Silent", "FakeGlass", "Shader")
    count = 0
    with zipfile.ZipFile(get(FAKE_GLASS, "fake-glass.zip")) as archive:
        for name in archive.namelist():
            inside = name.split("/", 1)[1] if "/" in name else ""
            if not inside.startswith("Shader/") or name.endswith("/"):
                continue
            path = os.path.join(target, inside[len("Shader/"):])
            os.makedirs(os.path.dirname(path), exist_ok=True)
            open(path, "wb").write(archive.read(name))
            count += 1
    print("installed Fake Glass:", count, "files")


def main():
    project = sys.argv[1]
    vpm(project, LTCGI)
    # its adapter for Light Volumes is written for version 3 of them: with 2.1.3 UdonSharp
    # refuses the script, and with that every Udon program in the project
    for extra in ("LightVolumes", "LightVolumes.meta"):
        path = os.path.join(project, "Packages", LTCGI[0], extra)
        if os.path.isdir(path):
            shutil.rmtree(path)
        elif os.path.exists(path):
            os.remove(path)
    vpm(project, VOLUMES)
    listing = os.path.join(project, "Packages", "manifest.json")
    manifest = json.load(open(listing, encoding="utf-8"))
    if "com.unity.editorcoroutines" not in manifest["dependencies"]:   # Light Volumes needs it
        manifest["dependencies"]["com.unity.editorcoroutines"] = "1.0.0"
        json.dump(manifest, open(listing, "w", encoding="utf-8"), indent=2)
    unity_package(project, MOCHIE)
    fake_glass(project)


if __name__ == "__main__":
    main()
