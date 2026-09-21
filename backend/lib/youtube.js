import { spawn } from "node:child_process";
import { Readable } from "node:stream";
import fs from "node:fs";

function findYtDlp() {
  if (process.env.YT_DLP_PATH && fs.existsSync(process.env.YT_DLP_PATH)) {
    return process.env.YT_DLP_PATH;
  }
  const defaultPenv = "C:\\Users\\NNH\\.platformio\\penv\\Scripts\\yt-dlp.exe";
  if (fs.existsSync(defaultPenv)) {
    return defaultPenv;
  }
  return "yt-dlp";
}

export function streamYouTubeAudio(query, req, res) {
  const ytdlpPath = findYtDlp();
  const searchPattern = query.startsWith("http") ? query : `ytsearch1:${query}`;
  console.log(`[YOUTUBE] Searching: "${query}" using ${ytdlpPath}`);

  const args = [
    "-g",
    "-f", "ba[ext=m4a]/ba",
    searchPattern,
    "--no-warnings",
    "--no-playlist",
  ];

  const ytdlp = spawn(ytdlpPath, args, { windowsHide: true });
  let stdoutData = "";
  let stderrData = "";
  let killed = false;

  const abortYtDlp = () => {
    if (!killed) {
      killed = true;
      try { ytdlp.kill(); } catch {}
    }
  };

  req.once("close", abortYtDlp);

  ytdlp.stdout.on("data", (chunk) => {
    stdoutData += chunk;
  });

  ytdlp.stderr.on("data", (chunk) => {
    stderrData += chunk;
  });

  ytdlp.on("error", (err) => {
    console.error("[YOUTUBE] Spawn error:", err);
    req.removeListener("close", abortYtDlp);
    if (!res.headersSent && !res.writableEnded) {
      res.writeHead(500, { "Content-Type": "application/json; charset=utf-8" });
      res.end(JSON.stringify({ error: "Không khởi động được yt-dlp" }));
    }
  });

  ytdlp.on("close", async (code) => {
    req.removeListener("close", abortYtDlp);
    if (killed || res.writableEnded) return;

    if (code !== 0) {
      console.error(`[YOUTUBE] yt-dlp exited with code ${code}: ${stderrData.trim()}`);
      if (!res.headersSent && !res.writableEnded) {
        res.writeHead(502, { "Content-Type": "application/json; charset=utf-8" });
        res.end(JSON.stringify({ error: "Không tìm thấy hoặc không trích xuất được bài hát YouTube" }));
      }
      return;
    }

    const streamUrl = stdoutData.split(/\r?\n/).find((line) => line.startsWith("http"));
    if (!streamUrl) {
      console.error("[YOUTUBE] No audio URL extracted from output:", stdoutData);
      if (!res.headersSent && !res.writableEnded) {
        res.writeHead(404, { "Content-Type": "application/json; charset=utf-8" });
        res.end(JSON.stringify({ error: "Không lấy được link stream YouTube" }));
      }
      return;
    }

    console.log(`[YOUTUBE] Found stream URL, proxying to ESP32...`);
    const controller = new AbortController();
    const abortFetch = () => controller.abort();
    req.once("close", abortFetch);

    try {
      const upstream = await fetch(streamUrl, {
        signal: controller.signal,
        headers: {
          "User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36",
        },
      });

      if (!upstream.ok) {
        console.error(`[YOUTUBE] Upstream returned status ${upstream.status}`);
        req.removeListener("close", abortFetch);
        if (!res.headersSent && !res.writableEnded) {
          res.writeHead(upstream.status, { "Content-Type": "application/json; charset=utf-8" });
          res.end(JSON.stringify({ error: "Lỗi khi tải stream âm thanh từ YouTube" }));
        }
        return;
      }

      const contentType = upstream.headers.get("content-type") || "audio/mp4";
      const contentLength = upstream.headers.get("content-length");
      const headers = {
        "Content-Type": contentType,
        "Accept-Ranges": "none",
        "Cache-Control": "no-cache, no-store",
        "Access-Control-Allow-Origin": "*",
      };
      if (contentLength) headers["Content-Length"] = contentLength;

      res.writeHead(200, headers);
      const nodeStream = Readable.fromWeb(upstream.body);
      nodeStream.pipe(res);

      nodeStream.on("error", (err) => {
        console.error("[YOUTUBE] Stream pipe error:", err.message);
        res.destroy();
      });

      res.on("close", () => {
        req.removeListener("close", abortFetch);
        controller.abort();
      });
    } catch (err) {
      req.removeListener("close", abortFetch);
      if (err.name !== "AbortError") {
        console.error("[YOUTUBE] Fetch exception:", err);
      }
      if (!res.headersSent && !res.writableEnded) {
        res.writeHead(502, { "Content-Type": "application/json; charset=utf-8" });
        res.end(JSON.stringify({ error: "Không kết nối được tới máy chủ YouTube" }));
      }
    }
  });
}
