// ============================================================================
// 学生成绩管理系统（控制台版 + 内置 HTTP 服务，网页前端共用同一份数据）
//
// 控制台功能:
//   1、录入学生信息   2、显示学生信息   3、删除学生信息
//   4、修改学生信息   5、查找学生信息   6、按总分排序   0、退出系统
//
// 网页端功能（http://127.0.0.1:8080/ ）:
//   表格展示 / 录入 / 修改 / 删除（带确认）/ 查找 / 排序 / 操作提示
//
// 用法:
//   sms.exe               控制台菜单 + HTTP 服务（默认端口 8080）
//   sms.exe --server      仅启动 HTTP 服务（供网页前端使用）
//   sms.exe --port 9000   指定端口
//
// 数据文件: students.txt  (UTF-8，每行: 学号,姓名,语文,数学,英语)
// 前端页面: index.html    (与本程序同目录)
// ============================================================================
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

namespace {

const char* kDataFile   = "students.txt";
const char* kIndexFile  = "index.html";
const int   kDefaultPort = 4399;

// ------------------------------------------------------------------ 数据模型
struct Student {
    std::string stuno;                 // 学号
    std::string name;                  // 姓名
    int chinese = 0;                   // 语文
    int math = 0;                      // 数学
    int english = 0;                   // 英语
    int total() const { return chinese + math + english; }
};

std::vector<Student> g_students;       // 全部学生（学号唯一）
std::mutex           g_mtx;            // 控制台线程与 HTTP 线程共用
int                  g_port = kDefaultPort;
SOCKET               g_listenSock = INVALID_SOCKET;

// ------------------------------------------------------------------ 小工具
std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (unsigned char)s[b] <= ' ') ++b;
    while (e > b && (unsigned char)s[e - 1] <= ' ') --e;
    return s.substr(b, e - b);
}

int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// 显示宽度（中文按 2 列计），用于控制台表格对齐
size_t utf8CharLen(const std::string& s, size_t i) {
    unsigned char c = (unsigned char)s[i];
    if (c >= 0xF0) return 4;
    if (c >= 0xE0) return 3;
    if (c >= 0xC0) return 2;
    return 1;
}

unsigned cpOf(const std::string& s, size_t i, size_t n) {
    unsigned cp = 0;
    for (size_t k = 0; k < n; ++k) {
        unsigned char c = (unsigned char)s[i + k];
        cp = (k == 0) ? (c & (0xFFu >> (n + 1))) : ((cp << 6) | (c & 0x3Fu));
    }
    return cp;
}

bool wideCp(unsigned cp) {
    return (cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0xA4CF) ||
           (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) ||
           (cp >= 0xFE30 && cp <= 0xFE4F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
           (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x20000 && cp <= 0x3FFFD);
}

size_t dispWidth(const std::string& s) {
    size_t w = 0;
    for (size_t i = 0; i < s.size();) {
        size_t n = utf8CharLen(s, i);
        if (i + n > s.size()) n = 1;
        w += wideCp(cpOf(s, i, n)) ? 2 : 1;
        i += n;
    }
    return w;
}

std::string pad(const std::string& s, size_t width) {
    size_t d = dispWidth(s);
    return d >= width ? s : s + std::string(width - d, ' ');
}

// 学号/姓名合法性：非空、限长、不能含逗号或控制字符（数据文件按逗号分隔）
bool validField(const std::string& s) {
    if (s.empty() || s.size() > 40) return false;
    for (char c : s)
        if (c == ',' || c == '\t' || (unsigned char)c < 0x20) return false;
    return true;
}

// ------------------------------------------------------------------ 持久化
int findStudentLocked(const std::string& stuno) {
    for (size_t i = 0; i < g_students.size(); ++i)
        if (g_students[i].stuno == stuno) return (int)i;
    return -1;
}

void saveLocked() {
    std::ofstream ofs(kDataFile, std::ios::binary);
    for (const Student& s : g_students)
        ofs << s.stuno << ',' << s.name << ',' << s.chinese << ','
            << s.math << ',' << s.english << '\n';
}

void loadData() {
    std::ifstream ifs(kDataFile);
    if (!ifs) return;
    std::string line;
    while (std::getline(ifs, line)) {
        if (line.empty()) continue;
        std::vector<std::string> f;
        std::stringstream ss(line);
        std::string item;
        while (std::getline(ss, item, ',')) f.push_back(item);
        if (f.size() != 5) continue;
        Student s;
        s.stuno = f[0]; s.name = f[1];
        s.chinese = atoi(f[2].c_str());
        s.math    = atoi(f[3].c_str());
        s.english = atoi(f[4].c_str());
        if (validField(s.stuno) && validField(s.name)) g_students.push_back(s);
    }
}

// ------------------------------------------------------------------ 控制台输入
// 读一行；返回 false 表示 EOF（程序结束）
bool readLine(const char* prompt, std::string& out) {
    std::printf("%s", prompt);
    std::fflush(stdout);
    char buf[2048];
    if (std::fgets(buf, sizeof buf, stdin) == nullptr) return false;
    std::string line(buf);
    // 超长行吃掉残余，避免污染下一次读取（不会死循环）
    if (line.find('\n') == std::string::npos) {
        int c;
        while ((c = getchar()) != EOF && c != '\n') {}
    }
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    out = trim(line);
    return true;
}

// 读取 0~100 的成绩；非数字/越界要求重输，不会死循环
bool readScore(const char* prompt, int& out) {
    std::string line;
    while (true) {
        if (!readLine(prompt, line)) return false;   // EOF
        if (line.empty()) { std::printf("输入不能为空，请重新输入。\n"); continue; }
        bool digits = true;
        for (char c : line)
            if (!std::isdigit((unsigned char)c)) { digits = false; break; }
        if (!digits) { std::printf("输入无效，请输入数字。\n"); continue; }
        long v = std::strtol(line.c_str(), nullptr, 10);
        if (v < 0 || v > 100) {
            std::printf("成绩必须在 0~100 之间，请重新输入。\n");
            continue;
        }
        out = (int)v;
        return true;
    }
}

// 读取成绩，直接回车表示保持不变（修改功能用）
bool readScoreKeep(const char* prompt, int oldVal, int& out) {
    std::string line;
    while (true) {
        if (!readLine(prompt, line)) return false;
        if (line.empty()) { out = oldVal; return true; }
        bool digits = true;
        for (char c : line)
            if (!std::isdigit((unsigned char)c)) { digits = false; break; }
        if (!digits) { std::printf("输入无效，请输入数字。\n"); continue; }
        long v = std::strtol(line.c_str(), nullptr, 10);
        if (v < 0 || v > 100) {
            std::printf("成绩必须在 0~100 之间，请重新输入。\n");
            continue;
        }
        out = (int)v;
        return true;
    }
}

// 读取 y/n
bool readYesNo(const char* prompt, bool& out) {
    std::string line;
    while (true) {
        if (!readLine(prompt, line)) return false;
        if (line.empty()) continue;
        if (line == "y" || line == "yes" || line == "是") { out = true;  return true; }
        if (line == "n" || line == "no"  || line == "否") { out = false; return true; }
        std::printf("输入无效，请输入 y 或 n。\n");
    }
}

// ------------------------------------------------------------------ 控制台展示
void printTable(const std::vector<Student>& list) {
    if (list.empty()) {
        std::printf("\n暂无学生信息。\n");
        return;
    }
    std::printf("\n");
    std::printf("%s%s%s%s%s%s\n",
                pad("学号",   12).c_str(), pad("姓名", 12).c_str(),
                pad("语文",    8).c_str(), pad("数学",  8).c_str(),
                pad("英语",    8).c_str(), pad("总分",  8).c_str());
    for (const Student& s : list) {
        char num[8][16];
        std::snprintf(num[0], 16, "%d", s.chinese);
        std::snprintf(num[1], 16, "%d", s.math);
        std::snprintf(num[2], 16, "%d", s.english);
        std::snprintf(num[3], 16, "%d", s.total());
        std::printf("%s%s%s%s%s%s\n",
                    pad(s.stuno, 12).c_str(), pad(s.name, 12).c_str(),
                    pad(num[0], 8).c_str(), pad(num[1], 8).c_str(),
                    pad(num[2], 8).c_str(), pad(num[3], 8).c_str());
    }
    std::printf("共 %zu 名学生。\n", list.size());
}

// ------------------------------------------------------------------ 控制台操作
void opAdd() {
    while (true) {
        std::string stuno, name;
        if (!readLine("请输入学号: ", stuno)) return;
        if (stuno.empty())            { std::printf("学号不能为空，请重新输入。\n"); continue; }
        if (stuno.size() > 20)        { std::printf("学号过长（最多 20 个字符），请重新输入。\n"); continue; }
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            if (findStudentLocked(stuno) >= 0) {
                std::printf("学号 %s 已存在，请重新输入。\n", stuno.c_str());
                continue;
            }
        }
        if (!readLine("请输入姓名: ", name)) return;
        if (name.empty())             { std::printf("姓名不能为空，请重新输入。\n"); continue; }
        if (!validField(name))        { std::printf("姓名含非法字符，请重新输入。\n"); continue; }

        Student s;
        s.stuno = stuno;
        s.name  = name;
        if (!readScore("请输入语文成绩(0~100): ", s.chinese)) return;
        if (!readScore("请输入数学成绩(0~100): ", s.math))    return;
        if (!readScore("请输入英语成绩(0~100): ", s.english)) return;

        {
            std::lock_guard<std::mutex> lk(g_mtx);
            if (findStudentLocked(stuno) >= 0) {
                std::printf("学号 %s 刚被占用，录入失败。\n", stuno.c_str());
                return;
            }
            g_students.push_back(s);
            saveLocked();
        }
        std::printf("录入成功：%s %s（总分 %d）。\n", name.c_str(), stuno.c_str(), s.total());
        return;
    }
}

void opShow() {
    std::lock_guard<std::mutex> lk(g_mtx);
    printTable(g_students);
}

void opDelete() {
    std::string stuno;
    if (!readLine("请输入要删除的学生学号: ", stuno)) return;
    if (stuno.empty()) { std::printf("学号不能为空。\n"); return; }

    std::lock_guard<std::mutex> lk(g_mtx);
    int idx = findStudentLocked(stuno);
    if (idx < 0) {
        std::printf("未找到学号为 %s 的学生。\n", stuno.c_str());
        return;
    }
    const Student& s = g_students[idx];
    std::printf("找到学生：%s %s（总分 %d）\n", s.name.c_str(), s.stuno.c_str(), s.total());
    char prompt[128];
    std::snprintf(prompt, sizeof prompt, "确认删除学号 %s 的学生吗? (y/n): ", stuno.c_str());
    bool yes = false;
    if (!readYesNo(prompt, yes)) return;
    if (!yes) {
        std::printf("已取消删除。\n");
        return;
    }
    g_students.erase(g_students.begin() + idx);
    saveLocked();
    std::printf("删除成功。\n");
}

void opModify() {
    std::string stuno;
    if (!readLine("请输入要修改的学生学号: ", stuno)) return;
    if (stuno.empty()) { std::printf("学号不能为空。\n"); return; }

    std::lock_guard<std::mutex> lk(g_mtx);
    int idx = findStudentLocked(stuno);
    if (idx < 0) {
        std::printf("未找到学号为 %s 的学生。\n", stuno.c_str());
        return;
    }
    Student& s = g_students[idx];
    std::printf("当前信息：学号 %s，姓名 %s，语文 %d，数学 %d，英语 %d，总分 %d\n",
                s.stuno.c_str(), s.name.c_str(), s.chinese, s.math, s.english, s.total());
    std::printf("（直接回车表示保持不变）\n");

    std::string tmp;
    if (!readLine("新姓名: ", tmp)) return;
    if (!tmp.empty()) {
        if (!validField(tmp)) { std::printf("姓名含非法字符，修改失败。\n"); return; }
        s.name = tmp;
    }
    int c = 0, m = 0, e = 0;
    if (!readScoreKeep("新语文成绩(0~100): ", s.chinese, c)) return;
    if (!readScoreKeep("新数学成绩(0~100): ", s.math,    m)) return;
    if (!readScoreKeep("新英语成绩(0~100): ", s.english, e)) return;
    s.chinese = c; s.math = m; s.english = e;
    saveLocked();
    std::printf("修改成功：%s %s（总分 %d）。\n", s.name.c_str(), s.stuno.c_str(), s.total());
}

void opSearch() {
    std::string kw;
    if (!readLine("请输入学号或姓名关键字: ", kw)) return;
    if (kw.empty()) { std::printf("关键字不能为空。\n"); return; }

    std::vector<Student> hits;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        for (const Student& s : g_students)
            if (s.stuno == kw || s.name.find(kw) != std::string::npos)
                hits.push_back(s);
    }
    std::printf("关键字 \"%s\" 查到 %zu 条结果。\n", kw.c_str(), hits.size());
    printTable(hits);
}

void opSort() {
    std::printf("\n请选择排序方式:\n1、按总分从高到低\n2、按总分从低到高\n");
    std::string line;
    while (true) {
        if (!readLine("请选择(1/2): ", line)) return;
        if (line == "1" || line == "2") break;
        std::printf("输入无效，请输入 1 或 2。\n");
    }
    std::lock_guard<std::mutex> lk(g_mtx);
    if (line == "1") {
        std::stable_sort(g_students.begin(), g_students.end(),
                         [](const Student& a, const Student& b) { return a.total() > b.total(); });
        std::printf("已按总分从高到低排序。\n");
    } else {
        std::stable_sort(g_students.begin(), g_students.end(),
                         [](const Student& a, const Student& b) { return a.total() < b.total(); });
        std::printf("已按总分从低到高排序。\n");
    }
    saveLocked();
    printTable(g_students);
}

void runConsole() {
    while (true) {
        std::printf("\n==================================================\n");
        std::printf("             欢迎来到学生成绩管理系统             \n");
        std::printf("==================================================\n");
        std::printf("请选择要操作的命令\n");
        std::printf("1、录入学生信息\n");
        std::printf("2、显示学生信息\n");
        std::printf("3、删除学生信息\n");
        std::printf("4、修改学生信息\n");
        std::printf("5、查找学生信息\n");
        std::printf("6、按总分排序\n");
        std::printf("0、退出系统\n");

        std::string line;
        if (!readLine("请输入命令编号: ", line)) return;   // EOF，退出
        if (line.empty()) { std::printf("输入不能为空，请输入 0~6 的数字。\n"); continue; }
        bool digits = true;
        for (char c : line)
            if (!std::isdigit((unsigned char)c)) { digits = false; break; }
        if (!digits) { std::printf("输入无效，请输入 0~6 的数字。\n"); continue; }
        int cmd = std::atoi(line.c_str());
        switch (cmd) {
            case 1: opAdd();    break;
            case 2: opShow();   break;
            case 3: opDelete(); break;
            case 4: opModify(); break;
            case 5: opSearch(); break;
            case 6: opSort();   break;
            case 0:
                std::printf("再见！\n");
                return;
            default:
                std::printf("没有该命令，请输入 0~6 的数字。\n");
        }
    }
}

// ------------------------------------------------------------------ JSON
void utf8Append(std::string& s, unsigned cp) {
    if (cp < 0x80) {
        s += (char)cp;
    } else if (cp < 0x800) {
        s += (char)(0xC0 | (cp >> 6));
        s += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        s += (char)(0xE0 | (cp >> 12));
        s += (char)(0x80 | ((cp >> 6) & 0x3F));
        s += (char)(0x80 | (cp & 0x3F));
    } else {
        s += (char)(0xF0 | (cp >> 18));
        s += (char)(0x80 | ((cp >> 12) & 0x3F));
        s += (char)(0x80 | ((cp >> 6) & 0x3F));
        s += (char)(0x80 | (cp & 0x3F));
    }
}

std::string jsonUnescape(const std::string& s) {   // s 为不含首尾引号的字符串内容
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\\' || i + 1 >= s.size()) { out += s[i]; continue; }
        char c = s[++i];
        switch (c) {
            case '"':  out += '"';  break;
            case '\\': out += '\\'; break;
            case '/':  out += '/';  break;
            case 'b':  out += '\b'; break;
            case 'f':  out += '\f'; break;
            case 'n':  out += '\n'; break;
            case 'r':  out += '\r'; break;
            case 't':  out += '\t'; break;
            case 'u': {
                if (i + 4 >= s.size()) { out += 'u'; break; }
                unsigned cp = 0; bool ok = true;
                for (int k = 1; k <= 4; ++k) {
                    int h = hexVal(s[i + k]);
                    if (h < 0) { ok = false; break; }
                    cp = cp * 16 + (unsigned)h;
                }
                if (!ok) { out += 'u'; break; }
                i += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 < s.size() && s[i + 1] == '\\' && s[i + 2] == 'u') {
                    unsigned lo = 0; bool ok2 = true;
                    for (int k = 3; k <= 6; ++k) {
                        int h = hexVal(s[i + k]);
                        if (h < 0) { ok2 = false; break; }
                        lo = lo * 16 + (unsigned)h;
                    }
                    if (ok2 && lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        i += 6;
                    }
                }
                utf8Append(out, cp);
                break;
            }
            default: out += c;
        }
    }
    return out;
}

// 在扁平 JSON 对象中按键取值；isNum 区分字符串与数字
bool jsonGet(const std::string& body, const std::string& key,
             std::string& strOut, double& numOut, bool& isNum) {
    std::string pat = "\"" + key + "\"";
    size_t p = body.find(pat);
    if (p == std::string::npos) return false;
    if (p > 0 && body[p - 1] != '{' && body[p - 1] != ',' && body[p - 1] != ' ') return false;
    p += pat.size();
    while (p < body.size() && std::isspace((unsigned char)body[p])) ++p;
    if (p >= body.size() || body[p] != ':') return false;
    ++p;
    while (p < body.size() && std::isspace((unsigned char)body[p])) ++p;
    if (p >= body.size()) return false;

    if (body[p] == '"') {
        std::string raw;
        size_t e = p + 1;
        while (e < body.size()) {
            if (body[e] == '\\' && e + 1 < body.size()) { raw += body[e]; raw += body[e + 1]; e += 2; continue; }
            if (body[e] == '"') break;
            raw += body[e];
            ++e;
        }
        if (e >= body.size()) return false;
        strOut = jsonUnescape(raw);
        isNum = false;
        return true;
    }
    size_t e = p;
    while (e < body.size() && body[e] != ',' && body[e] != '}' && body[e] != ' ') ++e;
    std::string tok = trim(body.substr(p, e - p));
    if (tok.empty()) return false;
    strOut = tok;
    numOut = std::strtod(tok.c_str(), nullptr);
    isNum = true;
    return true;
}

std::string jsonEscape(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char b[8];
                    std::snprintf(b, sizeof b, "\\u%04x", c);
                    out += b;
                } else {
                    out += (char)c;
                }
        }
    }
    return out;
}

std::string studentJson(const Student& s) {
    std::string st = jsonEscape(s.stuno);
    std::string nm = jsonEscape(s.name);
    char buf[512];
    std::snprintf(buf, sizeof buf,
                  "{\"stuno\":\"%s\",\"name\":\"%s\",\"chinese\":%d,\"math\":%d,\"english\":%d,\"total\":%d}",
                  st.c_str(), nm.c_str(), s.chinese, s.math, s.english, s.total());
    return buf;
}

// ------------------------------------------------------------------ HTTP 服务
struct HttpRequest {
    std::string method, target, path, query, body;
    std::map<std::string, std::string> headers;
};

bool sendAll(SOCKET s, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        int n = send(s, data.data() + sent, (int)std::min(data.size() - sent, (size_t)1 << 20), 0);
        if (n <= 0) return false;
        sent += (size_t)n;
    }
    return true;
}

void sendResponse(SOCKET cli, int code, const char* status,
                  const std::string& type, const std::string& body) {
    std::string r;
    r += "HTTP/1.1 "; r += std::to_string(code); r += ' '; r += status; r += "\r\n";
    r += "Content-Type: "; r += type; r += "\r\n";
    r += "Content-Length: "; r += std::to_string(body.size()); r += "\r\n";
    r += "Access-Control-Allow-Origin: *\r\n";
    r += "Access-Control-Allow-Methods: GET, POST, PUT, DELETE, OPTIONS\r\n";
    r += "Access-Control-Allow-Headers: Content-Type\r\n";
    r += "Cache-Control: no-store\r\n";
    r += "Connection: close\r\n\r\n";
    r += body;
    sendAll(cli, r);
}

void sendJson(SOCKET cli, int code, const char* status, const std::string& json) {
    sendResponse(cli, code, status, "application/json; charset=utf-8", json);
}

std::string urlDecode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            int h = hexVal(s[i + 1]), l = hexVal(s[i + 2]);
            if (h >= 0 && l >= 0) {
                out += (char)((h << 4) | l);
                i += 2;
                continue;
            }
        }
        out += (s[i] == '+') ? ' ' : s[i];
    }
    return out;
}

std::map<std::string, std::string> parseQuery(const std::string& q) {
    std::map<std::string, std::string> m;
    std::istringstream ss(q);
    std::string kv;
    while (std::getline(ss, kv, '&')) {
        if (kv.empty()) continue;
        size_t e = kv.find('=');
        if (e == std::string::npos) m[urlDecode(kv)] = "";
        else m[urlDecode(kv.substr(0, e))] = urlDecode(kv.substr(e + 1));
    }
    return m;
}

std::string readFileBinary(const char* path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return std::string();
    return std::string(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
}

// 从请求体取得成绩字段，失败返回错误信息（success=false）
bool parseScores(const std::string& body, Student& s, std::string& err) {
    std::string vs; double dv; bool isNum;
    const char* keys[3] = {"chinese", "math", "english"};
    int* targets[3]  = {&s.chinese, &s.math, &s.english};
    const char* names[3] = {"语文", "数学", "英语"};
    for (int i = 0; i < 3; ++i) {
        if (!jsonGet(body, keys[i], vs, dv, isNum) || !isNum) {
            err = std::string("缺少或非法的") + names[i] + "成绩";
            return false;
        }
        if (dv != (double)(int)dv || dv < 0 || dv > 100) {
            err = std::string(names[i]) + "成绩必须是 0~100 的整数";
            return false;
        }
        *targets[i] = (int)dv;
    }
    return true;
}

void handleApi(SOCKET cli, HttpRequest& req) {
    std::string path = req.path;

    if (req.method == "OPTIONS") {           // CORS 预检
        sendJson(cli, 204, "No Content", "");
        return;
    }

    if (path == "/api/students") {
        if (req.method == "GET") {
            auto q = parseQuery(req.query);
            std::string sortMode = q.count("sort") ? q["sort"] : "total_desc";
            std::string kw = q.count("q") ? q["q"] : "";
            std::vector<Student> list;
            {
                std::lock_guard<std::mutex> lk(g_mtx);
                list = g_students;
            }
            if (!kw.empty()) {
                std::vector<Student> hits;
                for (const Student& s : list)
                    if (s.stuno == kw || s.name.find(kw) != std::string::npos) hits.push_back(s);
                list.swap(hits);
            }
            if (sortMode == "total_asc") {
                std::stable_sort(list.begin(), list.end(),
                                 [](const Student& a, const Student& b) { return a.total() < b.total(); });
            } else if (sortMode == "stuno") {
                std::stable_sort(list.begin(), list.end(),
                                 [](const Student& a, const Student& b) { return a.stuno < b.stuno; });
            } else {   // total_desc 默认
                std::stable_sort(list.begin(), list.end(),
                                 [](const Student& a, const Student& b) { return a.total() > b.total(); });
            }
            std::string js = "{\"students\":[";
            for (size_t i = 0; i < list.size(); ++i) {
                if (i) js += ',';
                js += studentJson(list[i]);
            }
            js += "],\"count\":" + std::to_string(list.size()) + "}";
            sendJson(cli, 200, "OK", js);
            return;
        }
        if (req.method == "POST") {
            std::string vs, nm, err;
            double dv; bool isNum;
            if (req.body.find('{') == std::string::npos) {
                sendJson(cli, 400, "Bad Request", "{\"ok\":false,\"error\":\"请求体不是有效的 JSON\"}");
                return;
            }
            Student s;
            if (!jsonGet(req.body, "stuno", vs, dv, isNum) || isNum || vs.empty()) {
                sendJson(cli, 400, "Bad Request", "{\"ok\":false,\"error\":\"学号不能为空\"}");
                return;
            }
            s.stuno = vs;
            if (s.stuno.size() > 20) {
                sendJson(cli, 400, "Bad Request", "{\"ok\":false,\"error\":\"学号过长（最多 20 个字符）\"}");
                return;
            }
            if (!jsonGet(req.body, "name", nm, dv, isNum) || isNum || nm.empty()) {
                sendJson(cli, 400, "Bad Request", "{\"ok\":false,\"error\":\"姓名不能为空\"}");
                return;
            }
            s.name = nm;
            if (!validField(s.name)) {
                sendJson(cli, 400, "Bad Request", "{\"ok\":false,\"error\":\"姓名含非法字符\"}");
                return;
            }
            if (!parseScores(req.body, s, err)) {
                sendJson(cli, 400, "Bad Request",
                         "{\"ok\":false,\"error\":\"" + jsonEscape(err) + "\"}");
                return;
            }
            {
                std::lock_guard<std::mutex> lk(g_mtx);
                if (findStudentLocked(s.stuno) >= 0) {
                    sendJson(cli, 409, "Conflict", "{\"ok\":false,\"error\":\"学号已存在\"}");
                    return;
                }
                g_students.push_back(s);
                saveLocked();
            }
            sendJson(cli, 201, "Created",
                     "{\"ok\":true,\"student\":" + studentJson(s) + "}");
            return;
        }
        sendJson(cli, 405, "Method Not Allowed", "{\"ok\":false,\"error\":\"方法不被支持\"}");
        return;
    }

    // /api/students/<stuno>
    const std::string prefix = "/api/students/";
    if (path.rfind(prefix, 0) == 0) {
        std::string stuno = urlDecode(path.substr(prefix.size()));
        if (stuno.empty()) {
            sendJson(cli, 400, "Bad Request", "{\"ok\":false,\"error\":\"缺少学号\"}");
            return;
        }
        std::lock_guard<std::mutex> lk(g_mtx);
        int idx = findStudentLocked(stuno);
        if (req.method == "DELETE") {
            if (idx < 0) {
                sendJson(cli, 404, "Not Found", "{\"ok\":false,\"error\":\"未找到该学生\"}");
                return;
            }
            g_students.erase(g_students.begin() + idx);
            saveLocked();
            sendJson(cli, 200, "OK",
                     "{\"ok\":true,\"stuno\":\"" + jsonEscape(stuno) + "\"}");
            return;
        }
        if (req.method == "PUT") {
            if (idx < 0) {
                sendJson(cli, 404, "Not Found", "{\"ok\":false,\"error\":\"未找到该学生\"}");
                return;
            }
            Student& s = g_students[idx];
            std::string vs, nm, err;
            double dv; bool isNum;
            if (jsonGet(req.body, "name", nm, dv, isNum)) {
                if (isNum || nm.empty()) {
                    sendJson(cli, 400, "Bad Request", "{\"ok\":false,\"error\":\"姓名不能为空\"}");
                    return;
                }
                if (!validField(nm)) {
                    sendJson(cli, 400, "Bad Request", "{\"ok\":false,\"error\":\"姓名含非法字符\"}");
                    return;
                }
                s.name = nm;
            }
            Student probe = s;                    // 校验成绩用副本
            if (!parseScores(req.body, probe, err)) {
                sendJson(cli, 400, "Bad Request",
                         "{\"ok\":false,\"error\":\"" + jsonEscape(err) + "\"}");
                return;
            }
            s = probe;
            saveLocked();
            sendJson(cli, 200, "OK",
                     "{\"ok\":true,\"student\":" + studentJson(s) + "}");
            return;
        }
        sendJson(cli, 405, "Method Not Allowed", "{\"ok\":false,\"error\":\"方法不被支持\"}");
        return;
    }

    sendJson(cli, 404, "Not Found", "{\"ok\":false,\"error\":\"接口不存在\"}");
}

void handleConnection(SOCKET cli) {
    std::string buf;
    char tmp[8192];
    size_t headerEnd = std::string::npos;

    while (headerEnd == std::string::npos) {
        int n = recv(cli, tmp, sizeof tmp, 0);
        if (n <= 0) { closesocket(cli); return; }
        buf.append(tmp, (size_t)n);
        if (buf.size() > (1u << 20)) { closesocket(cli); return; }
        headerEnd = buf.find("\r\n\r\n");
    }

    HttpRequest req;
    size_t lineEnd = buf.find("\r\n");
    if (lineEnd == std::string::npos) { closesocket(cli); return; }
    {
        std::istringstream iss(buf.substr(0, lineEnd));
        iss >> req.method >> req.target;
    }
    size_t pos = lineEnd + 2;
    while (pos < headerEnd) {
        size_t e = buf.find("\r\n", pos);
        if (e == std::string::npos || e > headerEnd) e = headerEnd;
        std::string h = buf.substr(pos, e - pos);
        size_t c = h.find(':');
        if (c != std::string::npos) {
            std::string k = h.substr(0, c);
            for (char& ch : k) ch = (char)std::tolower((unsigned char)ch);
            req.headers[trim(k)] = trim(h.substr(c + 1));
        }
        pos = e + 2;
    }
    size_t qp = req.target.find('?');
    if (qp == std::string::npos) {
        req.path = req.target;
    } else {
        req.path  = req.target.substr(0, qp);
        req.query = req.target.substr(qp + 1);
    }
    size_t bodyStart = headerEnd + 4;
    size_t contentLen = 0;
    auto it = req.headers.find("content-length");
    if (it != req.headers.end()) contentLen = (size_t)strtoul(it->second.c_str(), nullptr, 10);
    while (buf.size() - bodyStart < contentLen) {
        int n = recv(cli, tmp, sizeof tmp, 0);
        if (n <= 0) break;
        buf.append(tmp, (size_t)n);
        if (buf.size() > (2u << 20)) break;
    }
    if (contentLen) req.body = buf.substr(bodyStart, contentLen);

    if (req.path == "/" || req.path == "/index.html" || req.path == "/sms" ||
        req.path == "/sms.html") {
        if (req.method != "GET" && req.method != "HEAD") {
            sendJson(cli, 405, "Method Not Allowed", "{\"ok\":false,\"error\":\"方法不被支持\"}");
        } else {
            std::string html = readFileBinary(kIndexFile);
            if (html.empty()) {
                sendResponse(cli, 404, "Not Found", "text/plain; charset=utf-8",
                             "\u7F3A\u5C11 index.html\uFF0C\u8BF7\u5C06\u7F51\u9875\u524D\u7AEF\u6587\u4EF6\u653E\u5230\u7A0B\u5E8F\u540C\u76EE\u5F55\u3002");
            } else {
                sendResponse(cli, 200, "OK", "text/html; charset=utf-8", html);
            }
        }
        closesocket(cli);
        return;
    }

    if (req.path.rfind("/api/", 0) == 0) {
        handleApi(cli, req);
        closesocket(cli);
        return;
    }

    if (req.path == "/favicon.ico") {
        sendResponse(cli, 204, "No Content", "image/x-icon", "");
        closesocket(cli);
        return;
    }

    sendJson(cli, 404, "Not Found", "{\"ok\":false,\"error\":\"接口不存在\"}");
    closesocket(cli);
}

void httpAcceptLoop() {
    while (true) {
        sockaddr_in peer;
        int len = sizeof peer;
        SOCKET cli = accept(g_listenSock, (sockaddr*)&peer, &len);
        if (cli == INVALID_SOCKET) break;
        std::thread(handleConnection, cli).detach();
    }
}

bool startHttpServer(int port) {
    WSADATA w;
    if (WSAStartup(MAKEWORD(2, 2), &w) != 0) return false;
    g_listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_listenSock == INVALID_SOCKET) return false;
    BOOL reuse = TRUE;
    setsockopt(g_listenSock, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof reuse);
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((u_short)port);
    if (bind(g_listenSock, (sockaddr*)&addr, sizeof addr) == SOCKET_ERROR) {
        closesocket(g_listenSock);
        g_listenSock = INVALID_SOCKET;
        return false;
    }
    if (listen(g_listenSock, 32) == SOCKET_ERROR) {
        closesocket(g_listenSock);
        g_listenSock = INVALID_SOCKET;
        return false;
    }
    std::thread(httpAcceptLoop).detach();
    return true;
}

void printUsage() {
    std::printf(
        "学生成绩管理系统\n"
        "用法:\n"
        "  sms.exe               控制台菜单 + HTTP 服务(默认端口 %d)\n"
        "  sms.exe --server      仅启动 HTTP 服务(供网页前端使用)\n"
        "  sms.exe --port N      指定端口\n"
        "  sms.exe --help        显示帮助\n", kDefaultPort);
}

}  // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    bool serverOnly = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--server") {
            serverOnly = true;
        } else if (a == "--port" && i + 1 < argc) {
            g_port = std::atoi(argv[++i]);
        } else if (a.rfind("--port=", 0) == 0) {
            g_port = std::atoi(a.c_str() + 7);
        } else if (a == "--help" || a == "-h") {
            printUsage();
            return 0;
        } else {
            std::printf("未知参数: %s\n", a.c_str());
            printUsage();
            return 1;
        }
    }
    if (g_port < 1 || g_port > 65535) {
        std::printf("端口无效: %d\n", g_port);
        return 1;
    }

    loadData();

    if (!startHttpServer(g_port)) {
        std::printf("\n[提示] HTTP 服务启动失败（端口 %d 可能被占用），仅使用控制台功能。\n", g_port);
    } else {
        std::printf("\n[提示] HTTP 服务已启动: http://127.0.0.1:%d/\n", g_port);
        std::printf("[提示] 控制台与网页共用同一份数据；关闭本窗口或输入 0 退出，网页服务同时停止。\n");
    }

    int code = 0;
    if (serverOnly) {
        std::printf("[提示] 服务器模式运行中，关闭本窗口或按 Ctrl+C 退出。\n");
        while (true) Sleep(1000);
    } else {
        runConsole();
    }

    {
        std::lock_guard<std::mutex> lk(g_mtx);
        saveLocked();
    }
    std::printf("数据已保存。再见！\n");
    std::fflush(stdout);
    ExitProcess((UINT)code);
}
