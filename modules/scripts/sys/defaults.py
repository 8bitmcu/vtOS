import os
import json

def apply():
    try:
        os.chdir('/flash')

        with open("WELCOME.md", "x") as f:
            f.write(f"# Welcome to vtOS!\n"
                    f"This is your pocket hackable terminal. A few things to get you started:\n"
                    f" \n"
                    f"## Keyboard Navigation\n"
                    f"- `w`/`s`: Most TUI applications in vtOS support navigation with the `w` (up) and `s` (down) keys\n"
                    f"- `W`/`S`: Some applications support page navigation with the `W` and `S` keys\n"
                    f"- `q`: Will quit most applications.\n"
                    f" \n"
                    f"## Trackball Navigation\n"
                    f"- `Up`/`Down`: send `up`/`down` keys; scroll terminal history\n"
                    f"- `Left`/`Right`: send `left`/`right` keys; scroll command history\n"
                    f"- `Click`: send `Escape`; acts as a back button from input fields\n"
                    f"- `Long Click`: send `KeyboardInterrupt`\n"
                    f" \n"
                    f"## Built-in Shell\n"
                    f"The device first boots into the MicroPython shell and then the built-in shell is launched"
                    f" after. The built-in shell can process commands and arguments, while the MicroPython shell"
                    f" can process MicroPython expressions.\n"
                    f"- Type `help` in the shell to get a list of applications.\n"
                    f"- Type `fav` in the shell to create a new alias.\n"
                    f"- Type `exit` to get out of the built-in shell, into the MicroPython shell.\n"
                    f"- Type `sh` from the MicroPython shell to get back into the built-in shell.\n"
                    f"- Create `.shellrc` in `/flash` or `/sd` to execute commands at every boot. This is useful"
                    f" for connecting to your network every time you boot. (`nm connect <ssid> <password>`).\n"
                    f" \n"
                    f"## Getting Started\n"
                    f"- Connect to your Wi-Fi network using `nm`\n"
                    f"- Browse files in your device using `fm`\n"
                    f"- Access `menu` for a list of interactive commands\n"
                    f"- Change your font with `fc`. Try `menu fc` for a list\n"
                    f"- Automate your Wi-Fi connection or font configuration on every boot with `.shellrc`\n"
                    f"- This device is also a server, not just a client!\n"
                    f" \n"
                    f"Happy hacking!")

        with open(".virc", "x") as f:
            f.write('''"vi themes, uncomment to use:
"theme solarized
"theme gruvbox
"theme catppuccin
"theme tokyonight
"theme nord
''')

        try:
            os.mkdir("menu")
        except:
            pass

        with open("menu/.md.json", "x") as f:
            json.dump({
                "WELCOME.md": "/flash/WELCOME.md"
        }, f)

        with open("menu/.rss.json", "x") as f:
            json.dump({
                "BBC News World": "http://feeds.bbci.co.uk/news/world/rss.xml",
                "The New York Times": "https://rss.nytimes.com/services/xml/rss/nyt/HomePage.xml",
                "CNN Top Stories": "http://rss.cnn.com/rss/cnn_topstories.rss",
                "NPR": "https://feeds.npr.org/1001/rss.xml",
                "Hacker News": "https://news.ycombinator.com/rss",
                "TechCrunch": "https://techcrunch.com/feed/",
                "Ars Technica": "https://feeds.arstechnica.com/arstechnica/index",
                "Hack A Day": "https://hackaday.com/blog/feed/",
                "NASA": "https://www.nasa.gov/news-release/feed/",
                "Nature": "https://www.nature.com/nature.rss",
                "MIT Technology Review": "https://www.technologyreview.com/feed/",
                "Quanta Magazine": "https://api.quantamagazine.org/feed/",
        }, f)

        with open("menu/.irc.json", "x") as f:
            json.dump({
                "Libera.Chat": "irc.libera.chat 6667",
                "OFTC": "irc.oftc.net 6667",
                "EFnet": "irc.efnet.org 6667",
                "Rizon": "irc.rizon.net 6667",
                "Undernet": "irc.undernet.org 6667",
                "DALnet": "irc.dal.net 6667",
                "QuakeNet": "irc.quakenet.org 6667",
                "IRCnet": "irc.ircnet.com 6667"
        }, f)

        with open("menu/.telnet.json", "x") as f:
            json.dump({
                "Telehack": "telehack.com",
                "RetroCampus": "bbs.retrocampus.com"
        }, f)

        with open("menu/.fc.json", "x") as f:
            json.dump({
                "Cozette 13px": "cozette_mpy_13",
                "Gohu 11px": "gohu_mpy_11",
                "Gohu 14px": "gohu_mpy_14",
                "Scientifica 10px": "scientifica_mpy_10",
                "Spleen 8px": "spleen_mpy_8",
                "Spleen 12px": "spleen_mpy_12",
                "Tazmen 11px": "tamzen_mpy_11",
                "Terminus 12px": "terminus_mpy_12",
                "Terminus 14px": "terminus_mpy_14",
                "Unifont 16px": "unifont_mpy_16",
        }, f)

        with open("menu/.loracfg.json", "x") as f:
            json.dump({
                "433 MHz (Asia & Global Alternative)": "433",
                "868 MHz (Europe)": "868",
                "915 MHz (North America & Australia)": "915",
        }, f)

        with open("menu/.gemini.json", "x") as f:
            json.dump({
                "Project Gemini (official site)": "gemini://geminiprotocol.net/",
                "Gemini software directory": "gemini://geminiprotocol.net/software/",
                "TLGS (search engine)": "gemini://tlgs.one/",
                "Kennedy (search engine)": "gemini://kennedy.gemi.dev/",
                "Flounder (free capsule hosting)": "gemini://flounder.online/",
                "mozz.us (personal capsule)": "gemini://mozz.us/",
                "rawtext.club (tilde community)": "gemini://rawtext.club/",
                "tilde.cafe (tilde community)": "gemini://tilde.cafe/",
                "Houston (capsule uptime check)": "gemini://houston.gmi.bacardi55.io/",
                "Random capsule hopper": "gemini://fumble-around.mediocregopher.com/",
                "OpenBSD ports browser": "gemini://gemini.omarpolo.com/cgi/gempkg/",
                "Lupa crawler stats": "gemini://gemini.bortzmeyer.org/software/lupa/stats.gmi",
                "Solderpunk (Gemini's creator)": "gemini://zaibatsu.circumlunar.space/~solderpunk/",
                "Cosmos (thread aggregator)": "gemini://cosmos.skyjake.fi/",
                "CAPCOM (random capsules/month)": "gemini://gemini.circumlunar.space/capcom/",
        }, f)

        with open("menu/.gopher.json", "x") as f:
            json.dump({
                "Floodgap (gopher root)": "gopher://gopher.floodgap.com/",
                "Veronica-2 (search portal)": "gopher://gopher.floodgap.com/1/v2",
                "Veronica-2 (search gopherspace)": "gopher://gopher.floodgap.com/7/v2/vs",
                "All known gopher servers": "gopher://gopher.floodgap.com/1/world",
                "SDF (Unix community gopherhole)": "gopher://sdf.org/",
                "Gopherpedia (Wikipedia gateway)": "gopher://gopherpedia.com/",
                "Dilbert + misc (telefisk.org)": "gopher://telefisk.org/",
                "Qualitative Idiosyncratic Xenodochium": "gopher://home.quix.us/",
                "CNN news feed (codevoid.de)": "gopher://codevoid.de/1/cnn",
                "Zaibatsu (Solderpunk's community)": "gopher://zaibatsu.circumlunar.space/",
                "tilde.town (tilde community)": "gopher://tilde.town/",
                "quux.org (long-running gopherhole)": "gopher://gopher.quux.org/",
                "Bitreich (gopher-centric community)": "gopher://bitreich.org/",
                "Gopher Project resources": "gopher://gopherproject.org/",
        }, f)

        with open("menu/.menu.json", "x") as f:
            json.dump({
                "BLE Chat": "blechat",
                "Chess": "chess",
                "Dictionary": "dict",
                "File Manager": "fm",
                "Font Config": "menu fc",
                "Gemini": "menu gemini",
                "Gopher": "menu gopher",
                "IRC": "menu irc",
                "LoRa Config": "menu loracfg",
                "LoRa Chat": "lorachat",
                "Minesweeper": "mines",
                "Network Manager": "nm",
                "Offline Wikipedia": "wiki",
                "RSS Reader": "menu rss",
                "Stream Radio": "menu stream",
                "Telnet": "menu telnet",
                "USB Mass Storage": "usbmsc",
                "vi": "vi",
        }, f)

        with open("menu/.stream.json", "x") as f:
            json.dump({
                "SomaFM: Groove Salad (Ambient/Downtempo)": "http://ice1.somafm.com/groovesalad-128-mp3",
                "SomaFM: DEF CON Radio (Hacker/Electronic)": "http://ice1.somafm.com/defcon-128-mp3",
                "SomaFM: Secret Agent (Lounge/Spy Music)": "http://ice1.somafm.com/secretagent-128-mp3",
                "181.fm: Energy 98 (Dance/Techno)": "http://listen.181fm.com/181-energy98_128k.mp3",
                "KEXP 90.3 FM Seattle (Indie/Alternative)": "http://live-mp3-128.kexp.org/kexp128.mp3",
                "Radio Paradise (Eclectic Rock/Pop)": "http://stream.radioparadise.com/mp3-128",
                "181.fm: The Eagle (Classic Rock)": "http://listen.181fm.com/181-eagle_128k.mp3",
                "181.fm: Awesome 80's (80s Pop/Rock)": "http://listen.181fm.com/181-awesome80s_128k.mp3",
                "Radio Swiss Jazz (Classic Jazz)": "http://stream.srg-ssr.ch/m/rsj/mp3_128",
                "WQXR 105.9 FM New York (Classical)": "http://stream.wqxr.org/wqxr.mp3",
                "181.fm: True Blues (Blues)": "http://listen.181fm.com/181-blues_128k.mp3",
                "181.fm: Power 181 (Top 40 Hits)": "http://listen.181fm.com/181-power_128k.mp3",
                "181.fm: The Mix (Variety Pop)": "http://listen.181fm.com/181-themix_128k.mp3",
                "181.fm: Old School HipHop/RnB (Hip-Hop)": "http://listen.181fm.com/181-oldschool_128k.mp3",
        }, f)

    except:
        pass

