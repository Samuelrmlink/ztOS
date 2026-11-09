# How ztMETRO writes its words

ztOS takes Windows Phone 8.1's Metro rules for capital letters as its own.
They apply to everything the shell, Settings and the ztMETRO apps write on
the screen. They do not apply to text that comes from data (app names,
package and store descriptions, contact names, messages, file names, what a
network or device calls itself), to log and journal lines, or to command-line
output: those keep their own spelling.

| What | Case | Examples |
|---|---|---|
| The app's title, small, at the top | ALL CAPS | SETTINGS, STORE, MESSAGES |
| Page titles and pivot headers (the big text) | all lowercase | date + region, lock screen, always-on display, wi-fi |
| List entries that open a page | all lowercase | wi-fi, bluetooth, flight mode, lock screen |
| Section headers inside a page | all lowercase | force press, notifications, theme |
| Switches, buttons, options, captions, slider labels | Sentence case | Set time automatically, 24-hour time, Vibration strength: 80%, On, Off |
| Descriptions, help, details under an entry, messages | Sentence case, normal punctuation | Shown whenever the screen is off. |
| Quick-action tiles' labels | ALL CAPS (WP 8.1's action centre) | WI-FI, BLUETOOTH, ALL SETTINGS |

Names:

- Proper and technical names keep their spelling in Sentence-case text:
  Wi-Fi, Bluetooth, USB-C, Android, Flathub, Waydroid.
- Inside lowercase titles and entries they go lowercase, as Windows Phone
  wrote "wi-fi" and "bluetooth".
- Abbreviations that cannot be read lowercase stay capitals even there:
  USB, PD, SIM, NFC, AOD, APN, LED, NTP, 4G, GPS. As WP's "cellular+SIM".
- "+" stands for "and" in titles, with a space each side: "date + region",
  "ringtones + sounds".

In code: a page title is the string given to `zmc_header` (lowercase); an
entry's title is `zmc_entry`'s first string (lowercase) and its detail its
second (Sentence case); `zmc_switch`, `zmc_picker` and buttons take Sentence
case. Section headers are lowercase.
