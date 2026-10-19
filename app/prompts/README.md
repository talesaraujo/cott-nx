# Button prompt pictures

Everything in `switch/` is from **Xelu's Free Controller & Key Prompts**
by Nicolae "Xelu" Berbece (Those Awesome Guys):
<https://thoseawesomeguys.com/prompts/>

Released into the public domain under **Creative Commons 0 (CC0)**. Thank
you, Xelu! Not game data. (Folder layout after Mind over Recomp's
`assets/prompts`.)

Only the Switch pictures the game's prompts need are here (one 100 x 100 PNG
per button, unchanged). `tools/prompt_icons.py` turns them into
`app/src/cott_prompt_icons_data.h` (the "no pack" icon gets a red "no" sign
drawn over the A there), and `app/src/cott_prompt_icons.cpp` puts them into
the game's own prompt pictures while it runs, by position (the 360's A shows
the Switch B, and so on).
