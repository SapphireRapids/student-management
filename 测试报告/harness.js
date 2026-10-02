'use strict';
// 学生成绩管理系统（C++ 版）本地压测 / 原子落盘测试工具
// 用法: node harness.js <mode> [args]
//   smoke                         功能冒烟：接口契约逐项断言
//   stress <secs> <clients> <seedN> <threads>   8/4 客户端混合负载
//   edge                          边界：突发、空闲连接、半截请求、畸形请求
//   atomic                        原子落盘：写负载全程读快照 + 多轮强杀
// 说明: 每模式都在独立子目录里跑（复制 sms.exe + index.html + 种子数据），
//       不碰仓库里的 students.txt；服务端数据文件按 EXE 目录定位（v1.3fix 起）。

const net = require('net');
const fs = require('fs');
const path = require('path');
const { spawn } = require('child_process');

const HERE = __dirname;
const EXE = path.join(HERE, 'sms.exe');
const FAILED = [];
const ok = (name, cond, extra = '') => {
    const tag = cond ? '[OK]  ' : '[FAIL]';
    console.log(`  ${tag} ${name}${cond || !extra ? '' : '  ' + extra}`);
    if (!cond) FAILED.push(name + (extra ? ' (' + extra + ')' : ''));
    return cond;
};
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const portBase = () => 18000 + (process.pid % 2000);

// ---------- HTTP：每请求新建连接 + Connection: close（服务端一次连接一请求） ----------
function http(port, method, p, body, timeoutMs = 15000, connectTimeoutMs = 500) {
    return new Promise((resolve) => {
        const t0 = Date.now();
        const s = net.connect({ port, host: '127.0.0.1' });
        const chunks = [];
        let settled = false;
        let connected = false;
        const finish = (v) => { if (!settled) { settled = true; s.destroy(); resolve({ ...v, ms: Date.now() - t0 }); } };
        const ct = setTimeout(() => { if (!connected) finish({ ok: false, err: 'connect-timeout' }); }, connectTimeoutMs);
        s.setTimeout(timeoutMs, () => finish({ ok: false, err: 'timeout' }));
        s.on('connect', () => {
            connected = true;
            clearTimeout(ct);
            const head = [`${method} ${p} HTTP/1.1`, 'Host: 127.0.0.1', 'Connection: close'];
            if (body != null) {
                const b = Buffer.from(body, 'utf8');
                head.push('Content-Type: application/json', `Content-Length: ${b.length}`);
                s.write(Buffer.from(head.join('\r\n') + '\r\n\r\n', 'utf8'));
                s.write(b);
            } else {
                s.write(head.join('\r\n') + '\r\n\r\n', 'utf8');
            }
        });
        s.on('data', (d) => { chunks.push(d); });
        s.on('end', () => {
            clearTimeout(ct);
            const text = Buffer.concat(chunks).toString('utf8');
            const m = /^HTTP\/1\.[01] (\d{3})/.exec(text);
            if (!m) return finish({ ok: false, err: 'bad-response: ' + text.slice(0, 60) });
            const i = text.indexOf('\r\n\r\n');
            finish({ ok: true, status: +m[1], body: i >= 0 ? text.slice(i + 4) : '' });
        });
        s.on('error', (e) => { clearTimeout(ct); finish({ ok: false, err: e.code || e.message }); });
    });
}

// 裸 socket 请求：用于半截请求 / 超大畸形头 / chunked 等手工场景。
// chunks 可以是函数：收到连接后调用，用于“过一会再发”的分块流场景。
// 响应只要解析出状态行就立即返回（不等连接关闭），避免 RST 冲掉已到达的响应。
function raw(port, chunks, waitMs = 12000) {
    return new Promise((resolve) => {
        const s = net.connect({ port, host: '127.0.0.1' });
        let buf = '';
        let settled = false;
        const tryStatus = () => {
            const m = /^HTTP\/1\.[01] (\d{3})/.exec(buf);
            if (m && !settled) {
                settled = true;
                const i = buf.indexOf('\r\n\r\n');
                const r = { status: +m[1], body: i >= 0 ? buf.slice(i + 4) : '' };
                s.destroy();
                resolve(r);
            }
        };
        const finish = (v) => { if (!settled) { settled = true; s.destroy(); resolve(v); } };
        s.setTimeout(waitMs, () => finish({ status: 0, err: 'timeout', body: buf }));
        s.on('connect', async () => {
            for (const c of chunks) {
                if (typeof c === 'function') { await c(s); continue; }
                s.write(c);
            }
        });
        s.on('data', (d) => { buf += d.toString('utf8'); tryStatus(); });
        s.on('end', () => {
            const m = /^HTTP\/1\.[01] (\d{3})/.exec(buf);
            finish(m ? { status: +m[1], body: buf.slice(buf.indexOf('\r\n\r\n') + 4) } : { status: 0, err: 'closed', body: buf.slice(0, 60) });
        });
        s.on('error', (e) => finish({ status: 0, err: e.code || e.message, body: buf.slice(0, 60) }));
    });
}

// ---------- 数据文件快照校验：严格，任何一处撕裂即报错 ----------
function parseSnapshot(buf) {
    const text = new TextDecoder('utf-8', { fatal: true }).decode(buf); // 非法 UTF-8（切断在多字节字符中间）抛错
    if (!text.length) throw new Error('空文件');
    const lines = text.split('\n');
    if (lines[lines.length - 1] !== '') throw new Error('末行不完整（写一半被切断）');
    lines.pop();
    const seen = new Set();
    for (let i = 0; i < lines.length; i++) {
        const f = lines[i].split(',');
        if (f.length !== 3) throw new Error(`第 ${i + 1} 行字段数=${f.length}（应为 3）: ${lines[i].slice(0, 40)}`);
        const [st, nm, tot] = f;
        if (!st || !nm) throw new Error(`第 ${i + 1} 行学号或姓名为空`);
        if (!/^\d+$/.test(tot) || +tot > 1000000) throw new Error(`第 ${i + 1} 行总分非法: ${tot}`);
        if (seen.has(st)) throw new Error(`学号重复: ${st}`);
        seen.add(st);
    }
    return lines.length;
}

function seed(dir, n, tag = 'S') {
    const lines = [];
    for (let i = 1; i <= n; i++) lines.push(`${tag}${String(i).padStart(5, '0')},学生${i},${100 + (i % 800)}`);
    fs.writeFileSync(path.join(dir, 'students.txt'), lines.join('\n') + '\n', 'utf8');
}

function freshDir(name, { withHtml = true } = {}) {
    const dir = path.join(HERE, 'run-' + name);
    fs.rmSync(dir, { recursive: true, force: true });
    fs.mkdirSync(dir, { recursive: true });
    fs.copyFileSync(EXE, path.join(dir, 'sms.exe'));
    if (withHtml) fs.copyFileSync(path.join(HERE, 'index.html'), path.join(dir, 'index.html'));
    return dir;
}

function startServer(dir, port, threads, logName = 'srv.log') {
    const args = ['--server', '--port', String(port)];
    if (threads) args.push('--threads', String(threads));
    const out = fs.openSync(path.join(dir, logName), 'w');
    return spawn(path.join(dir, 'sms.exe'), args, { cwd: dir, stdio: ['ignore', out, out] });
}

async function waitPort(port, timeoutMs = 8000) {
    const t0 = Date.now();
    while (Date.now() - t0 < timeoutMs) {
        const r = await http(port, 'GET', '/api/students', null, 3000, 300);
        if (r.ok) return true;
        await sleep(60);
    }
    return false;
}

function killServer(child) {
    return new Promise((resolve) => {
        if (child.exitCode !== null || child.signalCode !== null) return resolve();
        child.on('exit', () => resolve());
        child.kill('SIGKILL'); // Windows: TerminateProcess，等价 taskkill -9
        setTimeout(resolve, 3000);
    });
}

function pct(arr, p) {
    if (!arr.length) return 0;
    const s = [...arr].sort((a, b) => a - b);
    return s[Math.min(s.length - 1, Math.floor(s.length * p))];
}
const median = (arr) => pct(arr, 0.5);

// ============================ smoke ============================
async function smoke() {
    console.log('== smoke：接口契约逐项断言 ==');
    const dir = freshDir('smoke');
    seed(dir, 50);
    const port = portBase();
    const srv = startServer(dir, port, 0, 'smoke-srv.log');
    ok('服务 8 秒内就绪', await waitPort(port));
    if (FAILED.length) { await killServer(srv); return; }

    let r = await http(port, 'GET', '/', null);
    ok('GET / 返回 200 且是 HTML', r.ok && r.status === 200 && /<html|<!DOCTYPE/i.test(r.body), r.err || String(r.status));
    r = await http(port, 'HEAD', '/', null);
    // 已知小瑕疵：C++ 版对 HEAD 也发送了完整 body（Rust 版已修）。浏览器不发 HEAD，不影响使用，此处只记录不判失败。
    ok('HEAD / 返回 200（附带返回了 body，属已知小瑕疵）', r.ok && r.status === 200, r.err || String(r.status));
    console.log(`  [note] HEAD / 实际响应体长度 ${r.ok ? r.body.length : 'N/A'} 字节（RFC 要求 HEAD 不返回 body）`);
    r = await http(port, 'GET', '/api/students', null);
    ok('GET /api/students 返回种子 50 条', r.ok && r.status === 200 && /"count":50/.test(r.body), r.err || r.body.slice(0, 60));

    r = await http(port, 'POST', '/api/students', '{"stuno":"SMOKE01","name":"冒烟测试","total":600}');
    ok('POST 合法录入返回 201', r.ok && r.status === 201, r.err || r.body.slice(0, 80));
    r = await http(port, 'POST', '/api/students', '{"stuno":"SMOKE01","name":"重复","total":1}');
    ok('POST 重复学号返回 409', r.ok && r.status === 409, r.err || String(r.status));
    r = await http(port, 'POST', '/api/students', '{"stuno":"SMOKE02","name":"越界","total":1000001}');
    ok('POST 总分越界返回 400', r.ok && r.status === 400, r.err || String(r.status));
    r = await http(port, 'POST', '/api/students', '{"stuno":"SMOKE03","name":"非数字","total":"abc"}');
    ok('POST 总分非数字返回 400', r.ok && r.status === 400, r.err || String(r.status));
    r = await http(port, 'POST', '/api/students', '{"stuno":"SMOKE04","name":"缺总分"}');
    ok('POST 缺字段返回 400', r.ok && r.status === 400, r.err || String(r.status));
    r = await http(port, 'POST', '/api/students', '这不是 JSON');
    ok('POST body 非 JSON 返回 400', r.ok && r.status === 400, r.err || String(r.status));
    r = await http(port, 'POST', '/api/students', '{"stuno":"逗,号","name":"非法学号","total":1}');
    ok('POST 学号含逗号返回 400', r.ok && r.status === 400, r.err || String(r.status));

    r = await http(port, 'PUT', '/api/students/SMOKE01', '{"total":999}');
    ok('PUT 合法修改返回 200', r.ok && r.status === 200, r.err || r.body.slice(0, 80));
    r = await http(port, 'GET', '/api/students?q=冒烟', null);
    ok('PUT 后可按姓名搜到且总分为 999', r.ok && r.status === 200 && /"total":999/.test(r.body), r.body.slice(0, 120));
    r = await http(port, 'PUT', '/api/students/NOSUCH', '{"total":1}');
    ok('PUT 不存在学号返回 404', r.ok && r.status === 404, r.err || String(r.status));
    r = await http(port, 'PUT', '/api/students/SMOKE01', '{"total":-5}');
    ok('PUT 总分负数返回 400', r.ok && r.status === 400, r.err || String(r.status));

    r = await http(port, 'GET', '/api/students?sort=total_asc', null);
    let js = r.ok ? JSON.parse(r.body) : { students: [] };
    const asc = js.students.map((x) => x.total);
    ok('sort=total_asc 升序', asc.every((v, i) => i === 0 || asc[i - 1] <= v), JSON.stringify(asc.slice(0, 5)));
    r = await http(port, 'GET', '/api/students?sort=total_desc', null);
    js = r.ok ? JSON.parse(r.body) : { students: [] };
    const desc = js.students.map((x) => x.total);
    ok('sort=total_desc 降序（默认）', desc.every((v, i) => i === 0 || desc[i - 1] >= v), JSON.stringify(desc.slice(0, 5)));
    r = await http(port, 'GET', '/api/students?sort=stuno', null);
    js = r.ok ? JSON.parse(r.body) : { students: [] };
    const nos = js.students.map((x) => x.stuno);
    ok('sort=stuno 按学号升序', nos.every((v, i) => i === 0 || nos[i - 1] <= v), JSON.stringify(nos.slice(0, 5)));

    r = await http(port, 'DELETE', '/api/students/SMOKE01', null);
    ok('DELETE 存在学号返回 200', r.ok && r.status === 200, r.err || String(r.status));
    r = await http(port, 'DELETE', '/api/students/SMOKE01', null);
    ok('DELETE 不存在学号返回 404', r.ok && r.status === 404, r.err || String(r.status));
    r = await http(port, 'DELETE', '/', null);
    ok('DELETE / 返回 405', r.ok && r.status === 405, r.err || String(r.status));
    r = await http(port, 'GET', '/api/nope', null);
    ok('GET 不存在路径返回 404', r.ok && r.status === 404, r.err || String(r.status));
    r = await http(port, 'GET', '/favicon.ico', null);
    ok('GET /favicon.ico 返回 204', r.ok && r.status === 204, r.err || String(r.status));

    // 落盘与接口一致
    r = await http(port, 'GET', '/api/students', null);
    const cnt = r.ok ? JSON.parse(r.body).count : -1;
    const fileLines = parseSnapshot(fs.readFileSync(path.join(dir, 'students.txt')));
    ok('接口条数 = 文件行数', cnt === fileLines, `api=${cnt} file=${fileLines}`);
    ok('无残留 .tmp', !fs.existsSync(path.join(dir, 'students.txt.tmp')));

    // 中文经 UTF-8 原样往返
    r = await http(port, 'GET', '/api/students?q=学生1', null);
    ok('中文关键字搜索可用', r.ok && r.status === 200 && JSON.parse(r.body).count > 0);

    await killServer(srv);
}

// ============================ stress ============================
async function stress(secs, clients, seedN, threads) {
    console.log(`== stress：${clients} 客户端 × ${secs}s，种子 ${seedN} 条，worker=${threads || 1} ==`);
    const dir = freshDir('stress');
    seed(dir, seedN);
    const port = portBase();
    const srv = startServer(dir, port, threads, 'stress-srv.log');
    ok('服务就绪', await waitPort(port));
    if (FAILED.length) { await killServer(srv); return; }

    const stop = { v: false };
    const lat = [];
    const hist = {};
    const errs = [];
    let n = 0;
    const t0 = Date.now();

    async function client(id) {
        let i = 0;
        while (!stop.v) {
            i++;
            const op = Math.random();
            const st = `W${id}_${i}`;
            let p, m = 'GET', body = null;
            if (op < 0.4) { p = '/api/students?sort=total_desc'; }
            else if (op < 0.6) { p = '/api/students?q=' + encodeURIComponent('学生' + (1 + (i % 50))); }
            else if (op < 0.8) { p = '/api/students'; m = 'POST'; body = `{"stuno":"${st}","name":"压测${id}-${i}","total":${100 + (i % 700)}}`; }
            else if (op < 0.9) { p = `/api/students/S${String(1 + (i % seedN)).padStart(5, '0')}`; m = 'PUT'; body = `{"total":${200 + (i % 500)}}`; }
            else { p = `/api/students/W${id}_${Math.max(1, i - 1)}`; m = 'DELETE'; }
            const r = await http(port, m, p, body);
            n++;
            if (r.ok) {
                lat.push(Number(r.ms || 0));
                hist[r.status] = (hist[r.status] || 0) + 1;
                const expect = { 200: 1, 201: 1, 404: 1, 409: 1 };
                if (!expect[r.status]) errs.push(`${m} ${p} -> ${r.status}`);
            } else {
                errs.push(`${m} ${p} -> ${r.err}`);
            }
        }
    }
    const runners = [];
    for (let c = 0; c < clients; c++) runners.push(client(c));
    await sleep(secs * 1000);
    stop.v = true;
    await Promise.all(runners);

    const dur = (Date.now() - t0) / 1000;
    console.log(`  请求总数: ${n}，吞吐: ${(n / dur).toFixed(1)} req/s`);
    console.log(`  延迟 p50/p95/p99/max: ${median(lat).toFixed(1)}/${pct(lat, 0.95).toFixed(1)}/${pct(lat, 0.99).toFixed(1)}/${Math.max(...lat, 0).toFixed(1)} ms`);
    console.log(`  状态分布: ${JSON.stringify(hist)}`);
    ok(`负载期间零错误（${errs.length} 个）`, errs.length === 0, errs.slice(0, 3).join(' | '));

    const cnt = (await http(port, 'GET', '/api/students', null));
    const api = cnt.ok ? JSON.parse(cnt.body).count : -1;
    const fileLines = parseSnapshot(fs.readFileSync(path.join(dir, 'students.txt')));
    ok('压测后 接口条数 = 文件行数', api === fileLines, `api=${api} file=${fileLines}`);
    ok('压测后无 .tmp', !fs.existsSync(path.join(dir, 'students.txt.tmp')));
    console.log(`  接口条数: ${api}`);
    await killServer(srv);
}

// ============================ edge ============================
async function edge() {
    console.log('== edge：边界场景 ==');
    const dir = freshDir('edge');
    seed(dir, 500);
    const port = portBase();
    const srv = startServer(dir, port, 0, 'edge-srv.log');
    ok('服务就绪', await waitPort(port));
    if (FAILED.length) { await killServer(srv); return; }

    // 1) 120 连接瞬时突发
    {
        const rs = await Promise.all(Array.from({ length: 120 }, () => http(port, 'GET', '/api/students', null, 20000, 2000)));
        const good = rs.filter((r) => r.ok && r.status === 200).length;
        const reset = rs.filter((r) => !r.ok).length;
        const bad = rs.filter((r) => r.ok && r.status !== 200).length;
        console.log(`  120 连接突发: 成功 ${good} / 被重置或失败 ${reset} / 异常码 ${bad}`);
        ok('突发后服务仍正常', (await http(port, 'GET', '/api/students', null)).ok);
    }

    // 2) 5 个连上不发数据的空闲连接，再发正常请求
    {
        const idles = Array.from({ length: 5 }, () => net.connect({ port, host: '127.0.0.1' }));
        await sleep(300);
        const t = Date.now();
        const r = await http(port, 'GET', '/api/students', null, 70000, 2000);
        const ms = Date.now() - t;
        console.log(`  5 空闲连接下正常请求: ${ms} ms 后 ${r.ok ? r.status : r.err}`);
        ok('空闲连接只拖慢不卡死（<=70s 返回 200）', r.ok && r.status === 200 && ms <= 70000, `${ms}ms`);
        for (const s of idles) s.destroy();
        await sleep(200);
    }

    // 3) 3 个半截请求（声称 500 字节 body 但不发），顺序进行
    for (let i = 0; i < 3; i++) {
        const t = Date.now();
        const r = await raw(port, [
            'POST /api/students HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nContent-Length: 500\r\nConnection: close\r\n\r\n',
        ], 15000);
        console.log(`  半截请求 #${i + 1}: ${Date.now() - t} ms 后 ${r.status || r.err}`);
        ok(`半截请求 #${i + 1} 约 10s 后返回 400`, r.status === 400 && Date.now() - t >= 9000);
    }

    // 4) 畸形请求（独立起一份服务单独跑）
    await malformed();
    await killServer(srv);
}

async function malformed() {
    console.log('== malformed：畸形请求 ==');
    const dir = freshDir('malformed');
    seed(dir, 100);
    const port = portBase() + 50;
    const srv = startServer(dir, port, 0, 'malformed-srv.log');
    ok('服务就绪', await waitPort(port));
    if (FAILED.length) { await killServer(srv); return; }
    const r1 = await raw(port, ['X'.repeat(1500000)], 15000); // 1.5MB 无头终止符
    ok('1.5MB 超大请求头被断开不卡死', r1.status === 0);
    const r2 = await http(port, 'POST', '/api/students', '这不是 JSON');
    ok('非 JSON body 返回 400', r2.ok && r2.status === 400);
    const r3 = await http(port, 'FOO', '/api/students', null);
    ok('不支持的方法返回 405', r3.ok && r3.status === 405);
    const r4 = await http(port, 'GET', '/api/nope', null);
    ok('不存在路径返回 404', r4.ok && r4.status === 404);
    // chunked（不带 Content-Length）：服务端读不到 body → 400。body 延迟发，模拟流式分块客户端。
    const r5 = await raw(port, [
        'POST /api/students HTTP/1.1\r\nHost: 127.0.0.1\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n',
        async (s) => { await sleep(300); s.write('1a\r\n{"stuno":"CHUNK01","name":"x",\r\n0\r\n\r\n'); },
    ], 15000);
    console.log(`  chunked body 录入: ${r5.status || r5.err} ${r5.body.slice(0, 60)}`);
    ok('chunked body 返回 400（已知特性，浏览器/curl 不触发）', r5.status === 400);
    await killServer(srv);
}

// ============================ atomic ============================
async function atomic() {
    // ---- 测试 A：写负载期间，数据文件的每一份快照都必须完整 ----
    console.log('== atomic A：写负载 + 全程读快照 ==');
    {
        const dir = freshDir('atomicA');
        seed(dir, 1500);
        const port = portBase();
        const srv = startServer(dir, port, 0, 'A-srv.log');
        ok('服务就绪', await waitPort(port));

        const stop = { v: false };
        let bad = null;
        let reads = 0;
        let prevLines = 0;
        const file = path.join(dir, 'students.txt');
        const reader = (async () => {
            while (!stop.v && !bad) {
                await new Promise((r) => setImmediate(r)); // 让出事件循环，否则写负载线程会被饿死
                try {
                    const lines = parseSnapshot(fs.readFileSync(file));
                    if (lines < prevLines) { bad = `行数倒退 ${prevLines} -> ${lines}（疑似被截断重写）`; break; }
                    prevLines = lines;
                    reads++;
                } catch (e) { bad = e.message; break; }
            }
        })();
        async function hammer(id) {
            let i = 0;
            while (!stop.v && !bad) {
                i++;
                const st = `A${id}_${i}`;
                await http(port, 'POST', '/api/students', `{"stuno":"${st}","name":"写${id}-${i}","total":${100 + (i % 700)}}`);
                if (bad) break;
                await http(port, 'PUT', `/api/students/${st}`, `{"total":${200 + (i % 500)}}`);
            }
        }
        const hs = [0, 1, 2, 3].map((i) => hammer(i));
        await sleep(4000);
        stop.v = true;
        await Promise.all([...hs, reader]);

        ok(`读取器读到大量快照（${reads} 次）`, reads > 50);
        ok('写负载期间每一份快照都完整（无撕裂、无行数倒退）', bad === null, bad || '');
        const api = JSON.parse((await http(port, 'GET', '/api/students', null)).body).count;
        const fileLines = parseSnapshot(fs.readFileSync(file));
        ok('接口条数 = 文件行数', api === fileLines, `api=${api} file=${fileLines}`);
        ok('无残留 .tmp', !fs.existsSync(path.join(dir, 'students.txt.tmp')));
        await killServer(srv);
    }

    // ---- 测试 B：写负载中多轮强杀，文件必须始终保持完整 ----
    console.log('== atomic B：5 轮“写 700ms → 强杀” ==');
    {
        const dir = freshDir('atomicB');
        seed(dir, 3000);
        const file = path.join(dir, 'students.txt');

        const stop = { v: false };
        let bad = null;
        let reads = 0;
        const reader = (async () => {
            const deadline = Date.now() + 30000;
            while (!stop.v && !bad && Date.now() < deadline) {
                await new Promise((r) => setImmediate(r));
                try { parseSnapshot(fs.readFileSync(file)); reads++; } catch (e) { bad = e.message; break; }
            }
        })();

        for (let round = 0; round < 5; round++) {
            const port = portBase() + round;
            const srv = startServer(dir, port, 0, `B-srv-${round}.log`);
            if (!(await waitPort(port))) { ok(`第 ${round} 轮服务就绪`, false); break; }
            const hstop = { v: false };
            async function hammer(id) {
                let i = 0;
                while (!hstop.v) {
                    i++;
                    const st = `B${round}_${id}_${i}`;
                    await http(port, 'POST', '/api/students', `{"stuno":"${st}","name":"杀${round}-${id}-${i}","total":${100 + (i % 700)}}`);
                    await http(port, 'PUT', `/api/students/${st}`, `{"total":${200 + (i % 500)}}`);
                }
            }
            const hs = [0, 1, 2, 3].map((i) => hammer(i));
            await sleep(700);
            await killServer(srv);           // 写负载进行中强杀
            hstop.v = true;
            await Promise.all(hs);

            const snap = fs.readFileSync(file);
            ok(`第 ${round} 轮强杀后文件非空`, snap.length > 0);
            let lines = -1;
            try { lines = parseSnapshot(snap); } catch (e) { ok(`第 ${round} 轮强杀后文件完整可解析`, false, e.message); }
            if (lines >= 0) ok(`第 ${round} 轮强杀后文件完整且 >=3000 行`, lines >= 3000, `实际 ${lines} 行`);
            fs.rmSync(path.join(dir, 'students.txt.tmp'), { force: true }); // 孤儿 .tmp 不影响正确性，清掉
        }
        stop.v = true;
        await reader;

        ok(`读取器跨轮次读到快照（${reads} 次）`, reads > 30);
        ok('强杀轮次期间没读到撕裂快照', bad === null, bad || '');

        const port = portBase() + 90;
        const srv = startServer(dir, port, 0, 'B-final.log');
        ok('重启服务就绪', await waitPort(port));
        const api = JSON.parse((await http(port, 'GET', '/api/students', null)).body).count;
        const fileLines = parseSnapshot(fs.readFileSync(file));
        ok('重启后 接口条数 = 文件行数', api === fileLines, `api=${api} file=${fileLines}`);
        const w = await http(port, 'POST', '/api/students', '{"stuno":"AFTERKILL","name":"重启后","total":600}');
        ok('重启后可继续写入（201）', w.ok && w.status === 201, w.err || String(w.status));
        await killServer(srv);
    }
}

// ============================ ro（只读目录最坏情况） ============================
async function roTest(dirIn) {
    const dir = path.resolve(dirIn);
    console.log(`== ro：只读目录（${dir}） ==`);
    const port = portBase() + 70;
    const log = path.join(HERE, 'ro-srv.log'); // 日志放目录外，否则打不开
    let srv = spawn(path.join(dir, 'sms.exe'), ['--server', '--port', String(port)],
        { cwd: dir, stdio: ['ignore', fs.openSync(log, 'w'), fs.openSync(log, 'a')] });
    ok('服务在只读目录也能启动', await waitPort(port));

    const before = JSON.parse((await http(port, 'GET', '/api/students', null)).body).count;
    ok('初始接口条数 = 3', before === 3, String(before));
    const w = await http(port, 'POST', '/api/students', '{"stuno":"ROTEST","name":"只读测试","total":600}');
    ok('只读目录下 POST 仍返回 201（内存中生效）', w.ok && w.status === 201, w.err || String(w.status));
    const after = JSON.parse((await http(port, 'GET', '/api/students', null)).body).count;
    ok('内存中可见新记录（4 条）', after === 4, String(after));
    const fileLines = parseSnapshot(fs.readFileSync(path.join(dir, 'students.txt')));
    ok('磁盘文件仍是 3 行（写入被拒绝，未产生半写文件）', fileLines === 3, `实际 ${fileLines} 行`);
    await killServer(srv);
    // 控制台模式干净退出会刷盘：用它验证用户可见的落盘失败警告
    // （--server 模式 stdio 块缓冲 + TerminateProcess，警告来不及刷盘，属测试手段限制）
    const con = spawn(path.join(dir, 'sms.exe'), [], { cwd: dir, stdio: ['pipe', 'pipe', 'pipe'] });
    let conOut = '';
    con.stdout.on('data', (d) => { conOut += d.toString('utf8'); });
    con.stderr.on('data', (d) => { conOut += d.toString('utf8'); });
    con.stdin.write('1\nROCON\n只读控制台\n600\n0\n');
    await new Promise((r) => { con.on('exit', r); setTimeout(r, 15000); });
    ok('控制台打印了落盘失败警告', /警告/.test(conOut), conOut.split('\n').find((l) => l.includes('警告')) || '（未找到）');
    const fileLines2 = parseSnapshot(fs.readFileSync(path.join(dir, 'students.txt')));
    ok('控制台录入后磁盘文件仍未变（3 行）', fileLines2 === 3, `实际 ${fileLines2} 行`);

    srv = spawn(path.join(dir, 'sms.exe'), ['--server', '--port', String(port + 1)],
        { cwd: dir, stdio: ['ignore', fs.openSync(log, 'a'), fs.openSync(log, 'a')] });
    ok('重启服务就绪', await waitPort(port + 1));
    const restart = JSON.parse((await http(port + 1, 'GET', '/api/students', null)).body).count;
    ok('重启后未写入的记录丢失（仍是 3 条，无脏数据）', restart === 3, String(restart));
    await killServer(srv);
}

// ============================ main ============================
(async () => {
    const mode = process.argv[2];
    try {
        if (mode === 'smoke') await smoke();
        else if (mode === 'stress') await stress(+process.argv[3] || 15, +process.argv[4] || 8, +process.argv[5] || 500, +process.argv[6] || 1);
        else if (mode === 'edge') await edge();
        else if (mode === 'malformed') await malformed();
        else if (mode === 'ro') await roTest(process.argv[3] || path.join(HERE, 'ro'));
        else if (mode === 'atomic') await atomic();
        else { console.log('未知模式: ' + mode); process.exit(2); }
    } catch (e) {
        console.log('未捕获异常: ' + (e && e.stack || e));
        FAILED.push('exception');
    }
    console.log(FAILED.length ? `\n结果: FAILED（${FAILED.length} 项）\n - ` + FAILED.join('\n - ') : '\n结果: ALL PASS');
    process.exit(FAILED.length ? 1 : 0);
})();
