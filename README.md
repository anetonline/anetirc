
ANetIRC - IRC for dos BBSes that produce door.sys and use a fossil driver. WIN32 Bridge




                          ANET IRC 1.0 - README
                    IRC Client for DOS Bulletin Boards
================================================================================

ANET IRC is a full-featured Internet Relay Chat client designed specifically
for DOS BBS door launches.  The 16-bit DOS door (ANETIRC.EXE) handles all
user-facing display and FOSSIL I/O; a Win32 helper bridge
(ANETIRC_BRIDGE.EXE) carries the actual IRC network connection.

The two processes communicate via pairs of small text files in the BBS's
door working directory, so the door doesn't need a network stack and the
bridge doesn't need a DOS environment.


================================================================================
                             WHAT YOU GET
================================================================================

Standard IRC commands
---------------------
  /join #channel [key]     Join a channel (optionally with a key)
  /part [#channel]         Leave a channel
  /list [pattern]          Browse the channel directory
  /msg nick text           Send a private message
  /notice nick text        Send a NOTICE
  /me action               Send a CTCP ACTION
  /r text                  Reply to the last PM partner
  /nick newnick            Change nick
  /names [#chan]           List channel members
  /who [target]            WHO lookup
  /whois nick              WHOIS lookup
  /whowas nick             WHOWAS history
  /topic [text]            Read or set a channel topic
  /mode target flags       Set modes (channel or user)
  /umode flags             Shorthand for user modes
  /op /deop /voice /devoice nick   Ops shortcuts
  /ban /unban mask         Channel ban helpers
  /kick nick [reason]      Kick
  /invite nick [#chan]     Invite
  /away [msg] /back        AFK control
  /ison n1 [n2 ..]         Are these nicks online?
  /userhost nick           User+host lookup
  /motd /time /version     Server queries
  /lusers /info /links /stats /admin
  /raw <line>              Send a raw IRC line (advanced)

Services
--------
  /identify password       NickServ IDENTIFY
  /register pw [email]     NickServ REGISTER
  /ns COMMAND              Talk to NickServ
  /cs COMMAND              Talk to ChanServ
  /ms COMMAND              Talk to MemoServ

Client
------
  /quit [reason]           Disconnect
  /win #chan               Switch active channel window
  /twit /untwit nick       Manage the ignore list (max 10 nicks per user)
  /color 1-15              Change your nick color
  /prefix /suffix <text>   Add decoration around your nick
  /theme 1-6               Change the border color theme
  /ctcp nick COMMAND       Send a CTCP query
  /mentions                Paginated mention log
  /scroll up|down /pgup /pgdn  Scrollback control


================================================================================
                              REQUIREMENTS
================================================================================

DOS side (anetirc.exe):
  - 16-bit DOS (native or DOSEMU)
  - FOSSIL driver on the COM port the BBS uses (BNU, X00, etc.)
    The door reads line 1 of DOOR.SYS to pick the port.
  - Standard ANSI terminal on the caller's side.

Win32 side (anetirc_bridge.exe, config.exe):
  - Windows XP or newer (Schannel required for TLS).
  - TCP connectivity to the IRC server you configure.
  - Read/write access to the BBS door directory (for the bridge files).


================================================================================
                                BUILDING
================================================================================

Run the top-level driver:

  bash backup_and_build.sh

Or build individual components:

  cd dosdoor && bash build_fossil_dos.sh
  cd helper_win32 && bash helper_build_win32.sh

Toolchains:
  - DOS door  : OpenWatcom 2.0  (WATCOM=/path/to/openwatcom overrides)
  - Win32     : i686-w64-mingw32-gcc (CC=... overrides)


================================================================================
                                INSTALLING
================================================================================

See INSTALL.TXT for per-BBS specifics.  Quickstart:

  1. Put ANETIRC.EXE, ANETIRC_BRIDGE.EXE, and CONFIG.EXE in the door directory.
  2. Run CONFIG.EXE and fill in the server / nick / channel fields.
  3. Start ANETIRC_BRIDGE.EXE (optionally with --verbose) as a background
     service.  It polls the ANETIRC*.OUT files and handles every node that
     comes online.
  4. Configure the BBS door entry to run ANETIRC.EXE with DOOR.SYS.


================================================================================
                                 LICENSE
================================================================================

MIT License.  See LICENSE file.
