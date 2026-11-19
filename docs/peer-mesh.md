# Peer mesh, matchmaking, and bans

MeshCE matchmaking runs on the clients. There is no matchmaking server and no server browser. Each client listens for other clients, keeps a list of the ones it has met, and uses that list to fill a game. The client with the best connection hosts. The others join the invite that host already makes for a party.

The network version of this client is 41. Mac, Windows, and Linux speak the same packets. An older client that does not know this mesh drops the packets.

## Where the files and ports live

This client leaves the installed OpenCE data alone. Saves go in a separate folder:

| System | Folder |
| --- | --- |
| Mac | `~/Library/Application Support/MeshCE Mesh` |
| Windows | `%APPDATA%\MeshCE Mesh` |
| Linux | `$XDG_DATA_HOME/meshce-mesh`, or `~/.local/share/meshce-mesh` |

On Mac, the shader cache and shader reports go in `~/Library/Caches/MeshCE Mesh`. The identity file is `peer-identity.bin`. The saved peer list is `peers.dat`. The shared rank list is `ranks.dat` in that same save folder. Short votes to kick are kept in `strikes.dat`. Each client keeps a copy of every record it has accepted.

The game uses these UDP ports. None of them are the old game ports or the old matchmaking ports.

| Port | Use |
| --- | --- |
| 38950 | Game server |
| 38951 | Game client |
| 38952 | Mesh discovery |
| 38960 | Ranked playlist |
| 38961 | Free for all playlist |
| 38962 | 2v2 playlist |

This client does not open a connection to a matchmaking broker or a signalling broker, and it does not ask the old master server for a server list. The catalog of playlists is built on the client. An internet game is reached at the address in its invite link.

Two copies of the game on one machine cannot both listen on UDP 38952. Two hosts of the same playlist on one LAN can collide, because that playlist's game port is a real bind.

## Identity

The reputation id is an Ed25519 key created for this install and stored in `peer-identity.bin`. Every mesh packet starts with the public key and ends with a signature over the packet. A peer that cannot verify the signature drops the packet.

The same key always produces the same three names:

- The account is the hex of the first 16 bytes.
- The token is the hex of the last 16 bytes.
- The Mesh ID is `M` plus eight hex digits of the first four bytes, for example `M1A2B3C4`.

Whoever has the seed is that player. A new key is a new player, with a new Mesh ID and a clear ban record. The Mesh ID is derived from the key. No server assigns it, and changing the gamertag does not change it.

The Mesh ID is drawn after the gamertag in smaller characters wherever that name is a label: above a player, on the name you are looking at, on the scoreboard, in the voice list, in the lobby, and in chat. On the scoreboard the competitive level stays the gamertag's size and follows the Mesh ID. The kill feed keeps the gamertag alone. Until that install's key has arrived, the label is the gamertag by itself.

A gamertag is at most 11 characters. It has no leading or trailing space, it contains no `|`, and it contains at least one letter or digit. Printable ASCII is accepted, as are Latin letters from U+00C0 through U+017F. A name that fails those rules is rejected.

## The secure element

On a Mac, the client tries to seal the 32 byte seed to a P-256 key that stays in the Secure Enclave. The keychain item is tagged `meshce.peer.v1`. Wrapping uses ECIES. The Enclave signature uses ECDSA over SHA-256. When the seal succeeds, a copied identity file does not open on another Mac, because the other Mac does not have that Enclave key.

On Windows, the platform crypto provider keeps the keys in the TPM for this Windows user. The signing key is a P-256 key named `meshce.peer.v1`. The seed is sealed to a second TPM key named `meshce.peer.v1.seal`, with RSA-OAEP and SHA-256. Neither private key is exportable. On Linux, the client talks to `/dev/tpmrm0`, or to `/dev/tpm0` when that resource manager node is absent. It seals the seed under a storage key derived from that TPM, and it signs with a P-256 key derived from the same TPM. The private keys do not leave the TPM. A copied identity file does not open on another machine.

The client writes a sealed file only after it has unsealed that same blob and recovered the seed. A sealed file that this machine cannot open is left in place. The client does not replace it with a new key. Delete that file to start as a new player.

A hello can carry that P-256 public key and an ECDSA signature over the Ed25519 id. Mac, Windows, and Linux check that signature. A valid signature refreshes the time that peer was last seen. The hello also carries a bound flag, set only when this machine sealed the seed and the signature checks against that P-256 key. Receivers do not read the flag. The check does not change matchmaking, hosting, or bans.

The signature shows that the sender holds the P-256 private key in the hello. A modified program can generate a P-256 key in ordinary memory and sign with it. The TPM and the Enclave stop a copied identity file from opening on another computer. The app does not use Apple's App Attest service.

When the element is missing, or it refuses the key, the seed is stored as the raw 32 bytes and the bound flag stays clear. The game still runs. An ad hoc Mac build is the usual reason the Enclave refuses. On Linux, a TPM whose owner password is set also refuses, and the seed stays raw. The client writes `peer-tpm-wait` next to the identity file and asks again a day later, so the TPM lockout counter is stepped once a day at most. Delete that file to ask on the next launch. Mac, Windows, and Linux still accept one another's hellos. The Ed25519 signature is the check every platform shares. A machine with no element leaves the P-256 fields empty, and the others ignore empty fields.

## Finding other clients

Each client binds UDP 38952 and announces itself every 10 seconds. The announcement goes to the local broadcast addresses. Every other announcement carries the short peer list, and an announcement that is introducing someone carries that list too. The introducer is `bitmoda.ddns.net` on UDP 38952. That name is built into every client and looked up again every few minutes, or sooner when a lookup fails. The machine at that address runs MeshCE with UDP 38952 reachable from the internet, and it announces with the same hello and the same short peer list. A new install meets the peers that machine has heard. After a hello comes back, that machine is saved in `peers.dat` like any other peer. While a client hears fewer than 8 peers, a packet it is already sending also goes to the introducer about every 3 seconds, and a quiet client reaches the introducer with its hello. Once a client hears 8 peers, the introducer hears from it about once a minute. The hello that reaches the introducer carries the short peer list.

The saved list holds 2048 peers. While 64 or fewer addresses are saved, every saved address except the introducer gets each announcement. Once more are saved, each announcement goes to peers who just made contact, to a sample of peers heard in the last 30 seconds, and to the next slice of the saved list. The new contacts ride the hello, which carries the short peer list, and that queue walks so a later arrival is answered too. A search packet uses the sample and the slice. The slice keeps walking, so every saved address is tried in turn. The introducer is left out of both and contacted on its own schedule.

A peer that sends nothing for 45 minutes is dropped, and that slot can take someone who is online. A peer loaded from disk gets that same 45 minutes to answer before it is dropped. An address passed along by another client, which this client never hears, is dropped after 5 minutes. A ban that is still running is kept. A ban that has finished is kept until a new peer needs the slot. An invite that is still waiting for an answer is kept. When the list is full, a key this client has heard replaces the peer that has been quiet the longest. A peer heard in the last 30 seconds stays. A key that was only named in someone else's list does not replace a peer this client has heard.

A client that hears another client remembers that key, address, and port, and passes a short list of the peers it knows with every other announcement. An introduction carries that list too. The list is up to 8 peers this client has heard directly in the last 30 seconds, chosen at random each time from the operating system's random bytes. Peers are learned through other peers.

A join link also tells the two clients about each other once the join is accepted. Each one asks a STUN server from its mesh socket and sends that public mesh address on the game tunnel. Both then send hellos to that address. The game the host set up keeps running, and Find Game stays off. A peer is written into the saved list after a hello from that peer arrives. An address that never answers is left out of the saved list.

Bans are not copied in that peer list. A ban is applied only from a signed report, described below.

An address heard directly from a peer stands for 30 seconds. A list forwarded by someone else can fill in a peer this client has not heard, and it can move a peer that has been quiet for longer than that. It does not replace a fresh direct address.

This reaches as far as a path already exists. Two clients on the same LAN find each other by broadcast. A client that has met another one before tries that saved address again. A client with neither reaches `bitmoda.ddns.net`, and the peers that machine names are how those clients meet. The mesh does not add a relay. Two clients that still have no path between them after that introduction do not meet.

## What a search does

Matchmaking is a row on the multiplayer menu. The playlists are Free for all, 2v2, and Ranked. Choosing one opens a private lobby on this machine. Find Game is what starts the search. The private lobby does not start the match by itself.

The search runs in order:

1. For about a second the client announces that it is looking. It remembers who else announced the same playlist and the same network version.
2. It answers those looking broadcasts. As each peer answers that reply, it pings them, up to three times.
3. It trades scores with the peers who answered a ping.
4. The client with the best score hosts.

A client answers a looking broadcast, and that peer fills the lobby only after they answer that reply. The reply carries a nonce from the operating system's random bytes. Only an echo of that nonce fills a seat. Any other reply is ignored. The seat count and the address are taken from the looking broadcast that was answered, and a later broadcast does not move them. One address and port can fill one seat in that search. A peer this client has not played, and who is not already on the saved peer list, is answered only when their broadcast is at most a minute old. The search keeps answering those new peers while it pings, trades scores, and offers, up to the open seats. Peers already on that list, and peers already played, are drawn once from those who broadcast in the last two minutes. The draw uses the operating system's random bytes, and the replies go out together. A later looking broadcast fills a seat that is still open. A looking broadcast heard before Find Game still counts, for as long as it is inside that minute or those two minutes. While one of those replies has not come back, an offer from anyone else does not take this client. The reply is sent again until it is answered or a couple of seconds have passed, and then the seat is free.

The score is the worst answered round trip. The lowest worst time hosts. A client that answered nobody has 50 seconds added. Each ban on a key adds 10 seconds, so a banned key hosts when that total is still the lowest. A tie goes to the lower public key. Another client's score cannot be faster than the ping this client measured to them. The number of clients that score says it answered is not used. A client this one timed counts as one answer, and a client it did not time counts as none. A client that never answered a ping is not chosen.

The host makes a 16 byte match id and sends an offer every 2 seconds. The offer carries the playlist, the invite link, the map, the match id, and, for ranked, the search age. An offer that arrives while this client is still gathering is not the game. After that, an offer is the game only when this client has chosen that host, or when this client is still alone and would leave its own lobby for the sender. It is also the game when this client answered that sender's invite and has nobody else in its own search, so the two of them do not sit in separate lobbies. A search that already saw another client waits until it chooses. The other clients join that invite. The link names the host's address and its public mesh key, and the joiner sends the handshake to that address. The private key stays on the host. A regular internet game uses the same kind of link and does not change rank or games played. The game itself is a normal lobby on the playlist port. The mesh is how they agree who hosts and which match it is.

Ranked matching uses the level in the shared list for that key. A key with no record yet is level 0. The level written on an announcement is ignored. The gamertag on an announcement has to be a real gamertag. A name that fails those rules is not added to the search.

A client that is still alone when it decides can leave its own lobby for someone else's offer. In Free for all and 2v2, the higher key moves and the lower key stays host. In Ranked, a higher level stays host. Equal levels follow the same lower key rule. A player already sitting in the game keeps that host, including when the search itself never counted them. A second machine connected to that game keeps the host as well. A later offer leaves that lobby where it is, and the machine that just started searching joins it. A host whose game still has only its own players takes a lower key's offer at once. After that lobby has waited with only its own players, it takes an offer from someone already in its search, and that player joins the lobby the others are in.

A client that is waiting on a chosen host ignores offers from anyone else. If that offer never arrives, it waits 2 minutes, gathers once more, and then stops. A client in cooldown does not host. If nobody else can host, its search waits until the cooldown ends and then gathers again.

While the host is offering, a new peer who has not been played, and who answers the reply to a looking broadcast from the last minute, is added to the roster. The host refreshes the lobby about once a second, so the names and the start time keep up with the search.

## When the match starts

The host is the only machine that starts the countdown. Free for all is ready once two players are seated, and the lobby holds sixteen. 2v2 and Ranked are ready when all 4 seats are filled. A player searching alone does not start a match. When a lobby is ready, the host counts down 30 seconds and then starts. The same 30 seconds run the first time a search is ready and again after a match. A player sent to a lobby that is already full goes back to finding a match on that playlist.

After a matchmaking game, the carnage report stays up for 10 seconds. That wait is real time. A button that is already held when the report appears does not skip it. The host pressing it again returns everyone to the lobby. Another player stays on the report until then, and that button does not leave the match. The next map is chosen the same way as the first match, from a new match id. The count the lobby waits on is the people still in the game when the report ends. The host and everyone who joined see the 30 second countdown and the people sitting there. It starts when that many are seated. If someone leaves, the countdown stops and the open seat is offered to a player who is searching. The countdown starts again when that many players are seated. One more player than that does not start the 30 seconds over. A lobby that already has its players does not leave to join a different host.

Seats a machine advertises can make a 2v2 or Ranked search look full before those players are in the lobby. That hold does not apply to Free for all once two are seated. There are no service bots.

Each machine advertises how many local players it has, from 1 to 4. Ranked seats at most two of them on one machine, so the match is not one screen. That number counts toward the seats. The offer lists one name per machine. In 2v2 and Ranked the host's party is one team and the next party is the other. A later party goes to the lighter team. Extra local players on a joining machine take the team of a listed teammate on that machine, or the team of that machine's key, once the peer id arrives. The host's own players stay on the host team. This reseat runs during the lobby, before the match starts.

## The three playlists

The playlist id on the wire is the word the search compares. The name on screen is what the player sees. 2v2 is the on screen name of the playlist whose id is `hardcore`. That id is how clients recognize the same search. It is separate from the Hardcore custom game screen, which is its own set of server options and is not this playlist.

All three are slayer. Everyone spawns with an assault rifle and a pistol, except some Free for all matches. Respawn is 5 seconds. The motion sensor is off, and the radar shows no players. There is no death bonus and no kill penalty. The map comes from the first byte of the match id, so every client in that match loads the same one. Free for all uses every stock multiplayer map: Hang 'Em High, Prisoner, Battle Creek, Derelict, Chill Out, Sidewinder, Damnation, Rat Race, Boarding Action, Blood Gulch, Wizard, Chiron TL34, and Longest. 2v2 and Ranked use the first five of those.

| On screen | Wire id | Players | Game | Score | Port |
| --- | --- | --- | --- | --- | --- |
| Free for all | `ffa` | 16 | Free for all | 50 | 38961 |
| 2v2 | `hardcore` | 4, teams of 2 | Team slayer | 25 | 38962 |
| Ranked | `ranked` | 4, teams of 2 | Team slayer | 25 | 38960 |

About one Free for all match in three, when the match id says so, spawns a pistol and a sniper rifle instead of the assault rifle and pistol. 2v2 and Ranked always use the assault rifle and pistol.

In a Free for all match, every player hears every other player's voice. In 2v2 and Ranked, voice and text stay on the team. The lobby and the carnage report let everyone hear. A custom game keeps the host's own voice setting.

A finished Free for all or 2v2 adds one game played when every player is still in the match. Ranked does not add to games played. A match that finishes after a player has left changes no rank and adds no game, for the player who left and for everyone who stayed. When one human is left in a matchmaking game because the others have gone, the match ends. A tie changes no streak for the players who tied. In a free for all, two players sharing first stay as they are, and everyone else takes a loss.

## Ranked matchmaking

Ranked uses the same mesh as the other playlists, and it only groups players whose levels fit. The higher party's window has to cover the lower party's level. Equal levels cover each other at the start of the search.

Under level 50, the window opens on the ten the level sits in, and the top of the window is that party's own level. Level 23 opens at 20 through 23. At 50 and above, the window opens on a wider block. Level 50 is 50 through 50. Level 149 is 50 through 149. Level 200 is 50 through 200. Level 400 is 250 through 400. Level 500 is 350 through 500.

The window stays there for the first 5 minutes. It then steps down every 3 minutes. At 50 and above, the first step drops the floor by 100, or to 350 when the floor was already above that. Later steps drop the floor by 100 while it is above 50, and by 10 after that, and it stops at 0. Under 50, each step drops the floor by 10.

The window steps from the moment this search first heard that client. The age written on the announcement is not used. An empty search does not open the window ahead of the other client. Two clients hear each other within one announcement, so they open the same window.

Two solo ranked hosts resolve in one direction. The higher level keeps its lobby. The lower level moves, when that higher window covers it. Equal levels follow the lower public key, which stays host.

### Level

The level and the win and loss streaks are a record in the shared list, keyed by the Ed25519 id. The local card is updated from that record. Editing the local card does not change the list, and the next search uses the list.

| Level | Rank up | Rank down |
| --- | --- | --- |
| Below 50 | 2 wins | 3 losses, and 0 is the floor |
| 50 | 3 wins | A loss stays at 50 |
| 51 and above | 3 wins | 1 loss, and 50 is the floor |

A win clears the loss streak. A loss clears the win streak. Both streaks clear when the level changes. A tie changes nothing.

The result names who won when every player is still in the match. A player who left makes that result a no change. If any player has a team, team scores are added up and the higher team wins. A tie of the top teams changes nothing. In a free for all, the best place wins when places are present, and otherwise the highest score wins.

### How a result is applied

At the scoreboard, every client in the match calculates the same tally. The tally is the match id, the playlist, the install keys that played, and one line per player: name, team, score, place, and whether they quit. The keys and the lines are sorted, so the order they are stored in does not matter. A match that is not a team game stores -1 for the team, and the result is decided by place. The tally leaves out the account token and any quit row that only one machine still has on a card. A client waits to send until it has an install key for every machine on the scoreboard.

The client signs that tally and sends it on the mesh. A free for all of sixteen players is larger than one packet, so that scoreboard goes out in two pieces. The text those pieces rebuild is the same tally. Each client keeps the signed tallies for that match and passes them on, so a client that hears them late can count the same votes. A client's own tally is one of the votes. A tally from a key that was not in the match is kept and does not count.

The tally is accepted when more than half of the installs it names submitted that same text, and at least two did. Both clients in a two player match have to agree. Two of three is enough. Three of four is enough. Five of eight is enough. Nine of sixteen is enough. Two of four is a split, so the list stays as it is and nobody is banned for it. A client that never submits does not hold the others up. That match does not change the missing client's rank. Silence is not a ban. A shorter tally does not win while a longer one is present that itself has more than half, so two players cannot settle a four player match by leaving the others off the list. If a shorter tally was kept and a longer one later reaches that count, the rank change from the shorter one is taken back and the longer tally is kept. The mesh waits about 3 seconds after the latest tally before it keeps one, so the rest of the match can arrive. A client that was not in the match applies the same tally once it has those signed copies.

When a tally is accepted, each client updates its copy of the list from that tally, once per key. Each install line carries the gamertag, and that is the row for that key. The name written outside the tally is not used. The same gamertag on two install lines is left alone. On one machine with more than one player, the line uses the alphabetically first of those names, and every client writes that same line. The same key is not applied twice for one match. Free for all and 2v2 add one game played when every player is still in the match. Ranked updates the level and the streaks from the table above. A tally that marks anyone as gone updates neither.

A client whose tally differs from the accepted one is banned. A client that already submitted a longer tally is not, because that longer list may still be the match. That ban is one step on the table in the next section. The first one waits 1 hour, and the next one doubles. Silence is not a ban. The ban is how a client that submitted a different result is kept out of the next searches.

The list is saved in `ranks.dat`. A new client starts with an empty list and fills it from the tallies it sees. Lobby cards still show the level this client has for that name.

Badges are names for games played. They are computed on the client.

| Games played | Badge |
| --- | --- |
| 0 to 100 | Recruit |
| 101 to 200 | Regular |
| 201 to 400 | Veteran |
| 401 to 800 | Elite |
| 801 to 1600 | Legend |
| 1601 to 3200 | Mythic |
| 3201 to 6400 | Paragon |
| 6401 and above | Ascendant |

## Kick, report, and ban

A player starts a vote from the scoreboard or with `votekick` and a name. The others vote the same way. The vote runs for 45 seconds, and only one vote runs at a time. The host is the machine that counts. With the usual settings, a player other than the host needs 5 minutes on that host to start a vote, and 2 minutes to vote in one that is already open. The host's own players can vote, and they can be voted out the same way. A host does not ban, and a host does not kick on its own.

The vote removes the player when more than half of the players who may vote have voted, and at least two have. The player being voted on is counted in that half and does not vote for it. One player cannot kick the other. In a 4 player game, 3 votes are required. In an 8 player game, 5 are required. In a 16 player game, 9 are required. A team of four still cannot kick the other team. Two people cannot kick each other.

In the lobby, any seated player can vote to remove another machine. The control is on that player's row. The vote runs for 30 seconds. There is no time played wait, and a pass does not ban anyone or keep them out of a later game. They can join the lobby again. The person being voted on does not vote. When that leaves fewer people than the match count asks for, the others are enough, so two players can remove one another. With three or more, the count is the same as in a match. A pass against the host takes the host's own players out of the seats for that lobby. The next match can seat them again. A host can still remove a bot. A human is removed only by the vote.

A vote that fails makes the player who started it wait 5 minutes before starting another, and it protects the target for 10 minutes. Both of those waits are by address and hardware id on that host.

A vote to kick signs a report against the target's install key. The report is the text `MeshCE report v1`, the target key, the match id, and the host's install key. The host keeps the signatures. Clients tell the host their key with a peer id message, and they send the report on the match connection. Those messages are part of network version 41. A client that does not know them drops them. The report is sent again for a few seconds, because the host ignores it until that voter's key has arrived. Each new game clears the previous game's reports and machine ids. A report counts only when its signature matches the current match and the host named in it.

The signature names the host. The voter also sends that signature on the mesh, so the host is not the only copy. A signature checks for the host named when the vote was signed, so the same votes do not become a ban for a smaller game under someone else's key. The player count in the packet is not what decides the ban. The ban waits until the match tally is kept. A vote counts only when that install signed the tally, and the votes have to be more than half of the installs that tally names. A key that did not sign it cannot add a vote. The first player who left is the exception: the earliest quit on the kept tally is one vote against that install, and that player does not have to sign the tally. A player who leaves later is not a vote. Votes already signed against that later player still count when the tally shows they left. Players who leave on the same tick are all first. The host's tick is the one every client records. One leave by itself stays short of a ban. In a 4 player match, two signed votes and the leave are the three that ban. In an 8 player match, four signed votes and the leave are the five that ban. In a 16 player match, eight signed votes and the leave are the nine that ban. A game that never sends a tally does not start that cooldown. The host still keeps that player out of its own games.

### A vote that removes the player

The host publishes the mesh ban only once the vote has enough votes to remove the player. The reports have to reach that same count against the tally, not against a player count the host fills in. A shorter set does not ban the key from that match. The signed packet is sent on the mesh. The cooldown starts when the tally is kept and those votes are more than half of the installs it names. While this client is running, the same target and the same match are not banned twice.

That ban is stored with the peer in `peers.dat`. The first one waits 1 hour. Each further ban doubles the wait, and the wait stops growing at 7 days.

Leaving during the vote also removes the player from that host, as if the vote had passed. If the vote had already reached the count above, the mesh ban is published too. The report stays on the key the vote was using. A player who takes the open slot afterward is not the one named in that report.

### Votes that fall short

A vote that ends short of that count is still reported. The host sends the signatures when the vote fails, when the player leaves before it passes, and when the game ends with the vote still short. Each client that verifies a signature keeps it. It counts in `strikes.dat` once the tally is kept and that voter signed the tally. The same voter in the same match counts once. The same voter in a later match counts again. The first player who left adds one more, from the earliest quit on that tally. A player who leaves later does not. Votes already signed against that later player still count when the tally shows they left. Players who leave on the same tick are all first. The host's tick is the one every client records. The install key has to be on the tally. A crash and a quit are the same quit line, so both count. Two scoreboard rows with that name, or two installs with that name, do not add the vote. When the leave and the signed votes reach a ban, that leave is spent on the ban. When they stay short, the leave is one of the votes toward 33.

When 33 of those votes are stored against one key, that key is banned. The ban starts at the middle of the table below, 16 hours. A key already at that step or further takes the next step. Those 33 votes are then marked used, and the ban is saved with them. Another 33 are required for the next ban.

### The host's own keep-out

When the vote passes, or the player leaves during it, the host also keeps that address and hardware id out of its games. That wait uses the same table: 1 hour the first time, then double, up to 7 days. The count stays after the wait, so the next kick of that address or hardware id on that host is the next step. This list is the host's memory. It holds 32 of them. It is not the mesh ban, and it is gone when that host quits.

### The wait

The mesh ban is the one in the table. A majority's first ban, and a ban for a tally that does not match, wait 1 hour. The ban from 33 short votes starts at 16 hours. Each further ban doubles the wait, and the wait stops growing at 7 days.

| Bans | Wait before the key can be matched |
| --- | --- |
| 1 | 1 hour |
| 2 | 2 hours |
| 3 | 4 hours |
| 4 | 8 hours |
| 5 | 16 hours |
| 6 | 1 day 8 hours |
| 7 | 2 days 16 hours |
| 8 | 5 days 8 hours |
| 9 or more | 7 days |

During the wait, searches leave the key out. The player can sit in a search, and that search does not host and does not join anyone until the wait is over.

After the wait, the ban count remains. A search ignores the key for its first 6 seconds. After that, the key is added only when fewer than two people with a clear record are already in the group. The player who is searching counts as one of those when their own record is clear. A search that already has its host and one other clear player does not pick the banned player up. A search that is still alone after 6 seconds does.

A banned key that does get into a search has 10 seconds added to its host score for every ban. Another player hosts when their score is lower. The banned player hosts when their score is still the best, which is the case when nobody else answered.

A client applies a ban only from a report it verified. A client that missed that packet still treats the key as clear until a report reaches it.

## Limits

LAN broadcast, a join whose hello arrives, and the saved peer list are the ways two clients meet. A join tries the public mesh port between those two clients. A network that gives each destination its own port can still leave that pair unmet. The mesh does not add a relay.

The secure element seal is local to the machine that created the key. On a Mac that is the Secure Enclave. On Windows and Linux it is the TPM, when the machine has one. A copied identity file does not open on another machine. Other players identify the install by the Ed25519 signature. A new install is a new reputation. The P-256 signature shows that the sender holds that P-256 key. A modified program can create a P-256 key in ordinary memory.

The player count in a ban packet is not used. The tally is the roster, and only installs that signed it can vote. The earliest quit on that tally is a vote from the install that left first. A later quit is not a vote. Players who leave on the same tick are all first. A repeated ban packet after this client has quit can ban that match again. A vote that is still waiting for the tally does not count yet.

Short votes are kept until 512 are stored. A waiting vote is not dropped to make room for a different match's junk, and a match keeps its first sixteen waiting votes. A vote already used for a ban is the one that is forgotten when a slot is needed. A forgotten vote can count again if the same packet arrives. The host's keep-out forgets an expired count when a new player needs one of its 32 slots.

A rank tally counts only the keys that played in that match. Two of those keys can still agree with each other and fall short of a majority. They cannot change the list on their own when more than two machines played. A match that only those two installs ever report can still be kept, because a two player game is the two of them agreeing. A claimed search age does not widen the window. The window steps from the time this search has been hearing that client. A reply does not start that clock. A key named only in someone else's list does not take the slot of a peer this client has heard. A peer this client has heard can take the slot of the one that has been quiet the longest. A peer heard in the last 30 seconds stays. A peer that has been quiet for 45 minutes is dropped.

A client does not trust a host score that is faster than the ping it measured, and it does not trust how many other clients that score says it reached. A client that never answered a ping is not chosen to host. An offer does not make its sender the host while this client is still gathering, or while this client has already seen someone else and has not chosen yet.

The same gamertag on two installs is two Mesh IDs. The labels show that Mesh ID after the gamertag, in smaller characters, so the two installs can be told apart. A tally that has two rows with that name, or two keys claiming it, does not move either record.
