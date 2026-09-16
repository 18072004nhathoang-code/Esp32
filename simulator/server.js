const http = require('http');
const fs = require('fs');
const path = require('path');

const PORT = process.env.PORT || 8080;
const HOST = process.env.HOST || '127.0.0.1';
const BASE_DIR = __dirname;

const MIME_TYPES = {
    '.html': 'text/html; charset=utf-8',
    '.css': 'text/css; charset=utf-8',
    '.js': 'application/javascript; charset=utf-8',
    '.png': 'image/png',
    '.jpg': 'image/jpeg',
    '.svg': 'image/svg+xml',
    '.json': 'application/json'
};

function resolveRequestPath(rawUrl) {
    let reqPath = decodeURIComponent(rawUrl.split('?')[0]);
    if (reqPath === '/' || reqPath === '') {
        reqPath = '/index.html';
    }
    if (reqPath.includes('\0')) return null;

    const resolvedPath = path.resolve(BASE_DIR, `.${reqPath}`);
    const relativePath = path.relative(BASE_DIR, resolvedPath);
    if (relativePath === '..' || relativePath.startsWith(`..${path.sep}`) || path.isAbsolute(relativePath)) {
        return null;
    }
    return resolvedPath;
}

function createSimulatorServer() {
    return http.createServer((req, res) => {
        let safePath;
        try {
            safePath = resolveRequestPath(req.url);
        } catch (error) {
            res.writeHead(400, { 'Content-Type': 'text/plain; charset=utf-8' });
            return res.end('400 Bad Request');
        }

        if (!safePath) {
            res.writeHead(403, { 'Content-Type': 'text/plain; charset=utf-8' });
            return res.end('403 Forbidden');
        }

        fs.stat(safePath, (err, stats) => {
            if (err || !stats.isFile()) {
                res.writeHead(404, { 'Content-Type': 'text/plain; charset=utf-8' });
                return res.end('404 Not Found');
            }

            const ext = path.extname(safePath).toLowerCase();
            const contentType = MIME_TYPES[ext] || 'application/octet-stream';

            res.writeHead(200, {
                'Content-Type': contentType,
                'Access-Control-Allow-Origin': '*'
            });

            fs.createReadStream(safePath).pipe(res);
        });
    });
}

if (require.main === module) {
    const server = createSimulatorServer();
    server.listen(PORT, HOST, () => {
        console.log(`[Mini OS Simulator] Server is running at http://${HOST}:${PORT}/`);
        console.log(`[Mini OS Simulator] Serving directory: ${BASE_DIR}`);
    });
}

module.exports = { BASE_DIR, HOST, resolveRequestPath, createSimulatorServer };
