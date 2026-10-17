using UdonSharp;
using UnityEngine;
using UnityEngine.UI;
using VRC.SDKBase;

// Text from a field a visitor types or pastes into (docs/machine/console.md): it goes to a keyboard as
// if typed there, a little at a time, as the guest takes it. At the console's as characters,
// at the display's as key events, where a character no key gives is left out and counted.
[UdonBehaviourSyncMode(BehaviourSyncMode.None)]
public class EmuPaste : UdonSharpBehaviour
{
    public InputField field;
    public VRC.SDK3.Components.VRCUrlInputField urlField;   // or a link's field, shared with Open: its text typed as it is
    public EmuKeyboard keyboard;
    public Text status;

    private string pending = "";
    private int sent, skipped;

    private void Queue(string text)
    {
        if (text == null || text.Length == 0) return;
        text = text.Replace(((char)13).ToString(), "");   // (a pasted line may end in both)
        if (sent >= pending.Length)
        {
            pending = "";
            sent = 0;
        }
        pending += text;
    }

    // What the field holds, and the field emptied.
    private string Taken()
    {
        string text = "";
        if (field != null)
        {
            text = field.text;
            field.text = "";
        }
        else if (urlField != null)
        {
            text = urlField.GetUrl().Get();
            urlField.SetUrl(VRCUrl.Empty);
        }
        return text;
    }

    // The field's text, typed.
    public void Send()
    {
        Queue(Taken());
    }

    // And Return after it.
    public void SendLine()
    {
        Queue(Taken() + "\n");
    }

    // What has not been typed yet is dropped, and the field emptied.
    public void Drop()
    {
        pending = "";
        sent = 0;
        skipped = 0;
        Taken();
        Show();
    }

    public int Waiting()
    {
        return pending.Length - sent;
    }

    private void Show()
    {
        if (status != null) status.text = (Waiting() > 0 ? Waiting() + " to type" : "") + (skipped > 0 ? "  " + skipped + " left out" : "");
    }

    void Update()
    {
        if (sent >= pending.Length || keyboard == null) return;
        if (keyboard.rawKeys)
        {
            // key events: four a frame reach the guest, so a character a frame, two where no Shift is in the way
            if (keyboard.Count() > 8) return;
            for (int i = 0; i < 2 && sent < pending.Length; i++)
                if (!keyboard.TypeKey(pending[sent++])) skipped++;
            Show();
            return;
        }
        // the keyboard's queue holds 1,024: it is kept under a quarter of that, for the keys pressed
        // meanwhile (a classroom keyboard's goes into the machine's own 256: half of that)
        int room = (keyboard.sink != null ? 128 : 256) - keyboard.Count();
        if (room <= 0) return;
        int count = Mathf.Min(room, pending.Length - sent);
        keyboard.TypeText(pending.Substring(sent, count));
        sent += count;
        Show();
    }
}
