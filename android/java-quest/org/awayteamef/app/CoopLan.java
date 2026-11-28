package org.awayteamef.app;

import android.os.Build;

import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.InterfaceAddress;
import java.net.NetworkInterface;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Enumeration;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * "Search local network": finds co-op hosts on the LAN the way the desktop launchers do
 * (efcode/launcher/lc_coop.c, lc_coop_lan_scan; the host side is efcode/sys/sys_coop_host.c).
 *
 * The query is the connectionless packet FF FF FF FF "getcoopinfo <challenge> lan", sent over UDP to
 * port 29760 (the co-op port) at every network's broadcast address and at 255.255.255.255, twice
 * (at once and a third of the way in, in case a packet goes missing). A host answers the sender
 * with FF FF FF FF "coopInfoResponse\n" and an infostring (\key\value...) with coopProtocol, version,
 * hostname, player, state (playing / menu), map, mission, skill, players, maxPlayers, needPassword,
 * guest and the challenge, which must match. A host whose "Show this game on my network" is off
 * stays silent to the search. Nothing here touches the UI: {@link #scan} blocks for its timeout, so
 * call it from a worker thread.
 */
final class CoopLan {
    static final int PORT = 29760;
    /** = VSP_COOP_PROTOCOL and VSP_COOP_COMPAT (efcode/sys/sys_coop_net.h): what host and guest must share. */
    static final int PROTOCOL = 8;
    static final String COMPAT = "1.5.0";

    private static final byte[] OOB = {(byte) 0xff, (byte) 0xff, (byte) 0xff, (byte) 0xff};
    private static final String RESPONSE = "coopInfoResponse\n";

    /** One answering host. */
    static final class Game {
        String address = "";          // ip:port, what +set coop_connect takes
        Map<String, String> info = new HashMap<String, String>();

        String get(String key) {
            String v = info.get(key);
            return v == null ? "" : v;
        }

        boolean compatible() {
            return get("coopProtocol").equals(String.valueOf(PROTOCOL)) && get("version").equals(COMPAT);
        }

        boolean needsPassword() { return get("needPassword").equals("1"); }

        boolean full() { return get("guest").equals("1"); }

        /** Why this game can't be joined from here, or "" when it can (a password is asked for, not a problem). */
        String problem() {
            if (!compatible()) {
                String v = get("appVersion").isEmpty() ? get("version") : get("appVersion");
                return "Another version (host runs " + (v.isEmpty() ? "?" : v) + ")";
            }
            if (full()) return "Full: a friend already plays player 2";
            return "";
        }

        /** "Seven on MARINO-MAC", or the address when the host sent no names. */
        String title() {
            String player = get("player"), host = get("hostname");
            if (!player.isEmpty() && !host.isEmpty() && !player.equalsIgnoreCase(host))
                return player + " on “" + host + "”";
            if (!player.isEmpty()) return player;
            if (!host.isEmpty()) return host;
            return address;
        }

        /** "Assimilation (borg1) - Easy - 1 of 2 players - 192.168.1.20:29760". */
        String detail() {
            StringBuilder sb = new StringBuilder();
            String map = get("map"), mission = get("mission");
            if (!get("state").equals("playing") || map.isEmpty()) sb.append("In the menus");
            else if (!mission.isEmpty()) sb.append('“').append(mission).append("” (").append(map).append(')');
            else sb.append(map);
            int skill = parseInt(get("skill"), -1);
            String[] names = {"Easy", "Medium", "Hard"};
            if (skill >= 0) sb.append(" · ").append(skill < names.length ? names[skill] : "Skill " + skill);
            int players = Math.max(1, parseInt(get("players"), 1)), max = parseInt(get("maxPlayers"), 2);
            sb.append(" · ").append(players).append(" of ").append(max > 0 ? max : 2).append(" players");
            sb.append(" · ").append(address);
            return sb.toString();
        }
    }

    private CoopLan() {}

    static int parseInt(String s, int dflt) {
        try { return Integer.parseInt(s.trim()); } catch (RuntimeException e) { return dflt; }
    }

    /** An infostring "\k\v\k\v" into a map (later keys do not replace earlier ones, as the engine's lookup). */
    static Map<String, String> parseInfo(String s) {
        Map<String, String> m = new HashMap<String, String>();
        if (!s.startsWith("\\")) return m;
        String[] p = s.substring(1).split("\\\\", -1);
        for (int i = 0; i + 1 < p.length; i += 2)
            if (!m.containsKey(p[i])) m.put(p[i], p[i + 1]);
        return m;
    }

    /**
     * One datagram into the result list: null when it isn't an answer to this search, else the
     * game (new or the existing one for that address, updated). from = "ip:port".
     */
    static Game take(List<Game> games, String from, byte[] data, int len, String challenge) {
        byte[] head = (new String(OOB, StandardCharsets.ISO_8859_1) + RESPONSE).getBytes(StandardCharsets.ISO_8859_1);
        if (len <= head.length) return null;
        for (int i = 0; i < head.length; i++) if (data[i] != head[i]) return null;
        String text = new String(data, head.length, Math.min(len, 1024 + head.length) - head.length, StandardCharsets.UTF_8);
        Map<String, String> info = parseInfo(text);
        if (info.isEmpty()) return null;
        if (info.containsKey("challenge") && !info.get("challenge").equals(challenge)) return null;
        for (Game g : games) {
            if (g.address.equals(from)) {
                g.info = info;
                return g;
            }
        }
        Game g = new Game();
        g.address = from;
        g.info = info;
        games.add(g);
        return g;
    }

    /** The query packet for a challenge. */
    static byte[] query(String challenge) {
        byte[] t = ("getcoopinfo " + challenge + " lan").getBytes(StandardCharsets.ISO_8859_1);
        byte[] q = new byte[OOB.length + t.length];
        System.arraycopy(OOB, 0, q, 0, OOB.length);
        System.arraycopy(t, 0, q, OOB.length, t.length);
        return q;
    }

    /** The broadcast addresses of the up, non-loopback IPv4 interfaces (Wi-Fi, Ethernet), plus the limited one. */
    static List<InetAddress> targets() {
        List<InetAddress> out = new ArrayList<InetAddress>();
        try {
            Enumeration<NetworkInterface> nis = NetworkInterface.getNetworkInterfaces();
            if (nis != null) {
                for (NetworkInterface ni : Collections.list(nis)) {
                    if (!ni.isUp() || ni.isLoopback()) continue;
                    for (InterfaceAddress ia : ni.getInterfaceAddresses()) {
                        InetAddress b = ia.getBroadcast();
                        if (b instanceof Inet4Address && !out.contains(b)) out.add(b);
                    }
                }
            }
        } catch (Exception e) {
            // fall through to the limited broadcast
        }
        try {
            out.add(InetAddress.getByAddress(new byte[]{-1, -1, -1, -1}));
        } catch (Exception e) {
            // cannot happen: four bytes
        }
        // The Android emulator's NAT does not carry broadcasts; the host computer is 10.0.2.2 there.
        if (isEmulator()) {
            try { out.add(InetAddress.getByAddress(new byte[]{10, 0, 2, 2})); } catch (Exception e) { /* ignore */ }
        }
        return out;
    }

    private static boolean isEmulator() {
        return Build.FINGERPRINT.startsWith("generic") || Build.HARDWARE.contains("ranchu")
                || Build.HARDWARE.contains("goldfish") || Build.PRODUCT.contains("sdk");
    }

    /** Asks the network for {@code seconds} and returns the hosts that answered (blocks; not for the UI thread). */
    static List<Game> scan(double seconds, String testTargets) throws Exception {
        List<Game> games = new ArrayList<Game>();
        String challenge = String.format("%08x", new java.util.Random().nextInt());
        byte[] q = query(challenge);
        List<InetAddress> to = targets();
        if (testTargets != null && !testTargets.isEmpty())
            for (String t : testTargets.split(",")) to.add(InetAddress.getByName(t.trim()));
        DatagramSocket sock = new DatagramSocket();
        try {
            sock.setBroadcast(true);
            long start = System.nanoTime(), deadline = start + (long) (seconds * 1e9);
            byte[] buf = new byte[1500];
            int round = 0, sent = 0;
            IllegalStateException lastError = null;
            for (;;) {
                long now = System.nanoTime();
                if (now >= deadline) break;
                if (round < 2 && now - start >= (long) (seconds * 1e9 * round / 3)) {
                    for (InetAddress a : to) {
                        try {
                            sock.send(new DatagramPacket(q, q.length, a, PORT));
                            sent++;
                        } catch (Exception e) {
                            lastError = new IllegalStateException(e.getMessage() == null ? e.toString() : e.getMessage());
                        }
                    }
                    round++;
                }
                long next = round < 2 ? start + (long) (seconds * 1e9 * round / 3) : deadline;
                int wait = (int) Math.max(1, (next - System.nanoTime()) / 1000000L + 1);
                sock.setSoTimeout(wait);
                DatagramPacket p = new DatagramPacket(buf, buf.length);
                try {
                    sock.receive(p);
                } catch (java.net.SocketTimeoutException e) {
                    continue;
                }
                take(games, p.getAddress().getHostAddress() + ":" + p.getPort(), p.getData(), p.getLength(), challenge);
            }
            if (sent == 0 && lastError != null) throw lastError;
        } finally {
            sock.close();
        }
        return games;
    }
}
