// ============================================================================
//  EasyGitPusher - Um pequeno utilitário Win32 para subir uma pasta local
//  para um repositório do GitHub com um clique.
//
//  Como compilar no Visual Studio 2022+:
//    1. Crie um projeto "Aplicativo de Área de Trabalho do Windows" (C++)
//    2. Substitua o conteúdo do arquivo .cpp gerado por este arquivo
//    3. Compile e execute (Ctrl+F5)
//
//  Requisitos:
//    - Git precisa estar instalado e acessível no PATH do sistema
//      (baixe em https://git-scm.com se ainda não tiver)
//    - (Opcional) git-lfs para arquivos grandes (>50MB)
//
//  O que o programa faz:
//    - Barra lateral esquerda ("Meus repos") com os repositórios salvos.
//      Clicar num item preenche automaticamente Pasta / Link / Token.
//      O botão verde "+" salva a configuração atual dos campos como um
//      novo repositório na lista. A lixeira remove um repositório da lista
//      (não apaga nada no GitHub nem no disco, só tira da lista aqui).
//    - Ao clicar em "Push", roda os comandos git necessários
//      (init/add/commit/push) usando o token para autenticar, em uma
//      thread separada (a janela não trava durante o processo).
//    - Detecta automaticamente arquivos >50MB e configura Git LFS,
//      convertendo só os arquivos grandes para pointer LFS (não mexe
//      no resto do repositório nem reescreve histórico).
//
//  IMPORTANTE - sobre o token:
//    O token fica salvo em config.ini com uma ofuscação simples (XOR + Base64),
//    apenas para não ficar em texto puro "à vista" se alguém abrir o arquivo
//    casualmente. Isso NÃO é criptografia forte - qualquer pessoa com acesso
//    ao arquivo e um pouco de conhecimento pode recuperar o token. A segurança
//    real depende de quem tem acesso a essa pasta no seu computador. Se quiser
//    segurança de verdade, considere usar o Windows Credential Manager em vez
//    de um arquivo local (posso adaptar se precisar).
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#include <cstdarg>
#include <sal.h>
#include <windows.h>
#include <windowsx.h>
#include <ShObjIdl_core.h>
#include <commctrl.h>
#include <shellapi.h>   // ShellExecuteW (abre URL do logo no navegador)
#include <process.h>
#include <memory>
#include <string>
#include <sstream>    // std::wistringstream (parsing do git ls-remote)
#include <vector>
#include <atomic>
#include <gdiplus.h>
#include "resource.h"   // IDB_LOGO (logo embutido no EXE)

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "gdiplus.lib")

// Mensagem customizada: avisa a UI que a thread de push terminou
constexpr UINT WM_PUSH_DONE = WM_APP + 1;

// ---------------------------------------------------------------------------
//  IDs dos controles da janela
// ---------------------------------------------------------------------------
constexpr int ID_EDIT_FOLDER = 1001;
constexpr int ID_BTN_BROWSE = 1002;
constexpr int ID_EDIT_REPO = 1003;
constexpr int ID_EDIT_TOKEN = 1004;
constexpr int ID_CHK_SHOWTOKEN = 1005;
constexpr int ID_BTN_SAVE = 1006;
constexpr int ID_BTN_PUSH = 1007;
constexpr int ID_EDIT_LOG = 1008;
constexpr int ID_EDIT_COMMITMSG = 1009;
constexpr int ID_LIST_REPOS = 1010;
constexpr int ID_BTN_ADD_REPO = 1011;
constexpr int ID_COMBO_BRANCH = 1012;
constexpr int ID_STATIC_FOOTER = 1013;

// ---------------------------------------------------------------------------
//  Layout da barra lateral / deslocamento do conteúdo principal
// ---------------------------------------------------------------------------
constexpr int SIDEBAR_X = 15;
constexpr int SIDEBAR_WIDTH = 210;
constexpr int MAIN_X = SIDEBAR_X + SIDEBAR_WIDTH + 21; // -4 pixels (alinhamento) // conteúdo principal começa aqui
constexpr int TRASH_ICON_W = 30; // largura da área clicável da lixeira, dentro de cada item da lista

// ---------------------------------------------------------------------------
//  Handles globais dos controles
// ---------------------------------------------------------------------------
static HWND g_hEditFolder = nullptr;
static HWND g_hEditRepo = nullptr;
static HWND g_hEditToken = nullptr;
static HWND g_hChkShowToken = nullptr;
static HWND g_hEditLog = nullptr;
static HWND g_hEditCommitMsg = nullptr;
static HWND g_hMainWnd = nullptr;
static HWND g_hListRepos = nullptr;
static HWND g_hBtnAddRepo = nullptr;

static HFONT g_hFontRegular = nullptr;
static HFONT g_hFontBold = nullptr;
static HFONT g_hFontTitle = nullptr;   // "EasyGitPusher" (size 20 black)
static HFONT g_hFontLog = nullptr;     // Log box (size 12 bold blue)

// Logo Persia Studio no rodape da sidebar (clicavel -> abre o site)
static Gdiplus::Image* g_logoImage = nullptr;
static HWND g_hwndLogo = nullptr;
static IStream* g_logoStream = nullptr;   // mantem o stream vivo enquanto a imagem existir
static HINSTANCE g_hInstance = nullptr;   // salva para LoadPngFromResource
static constexpr wchar_t LOGO_URL[] = L"https://oficialgdg.wixsite.com/persiastudio";

// Background da janela (esticado para preencher toda a area cliente).
static Gdiplus::Image* g_bgImage = nullptr;
static IStream* g_bgStream = nullptr;
// Cor do texto dos labels (STATIC) sobre o fundo - azul ciano #00B6FF.
static constexpr COLORREF LABEL_TEXT_COLOR = RGB(0, 182, 255);
// Cor de fundo dos EDITs (cinza claro, combina melhor com fundo escuro).
static constexpr COLORREF EDIT_BG_COLOR = RGB(0x21, 0x21, 0x21);  // #212121 (dark)
static HBRUSH g_hbrEditBg = nullptr;   // brush reutilizavel para WM_CTLCOLOREDIT

// Sprites do botao "+" (3 estados) e da lixeira (3 estados).
// Indices: 0=normal, 1=highlight (hover), 2=clicado (pressed).
static Gdiplus::Image* g_sprPlus[3]   = { nullptr, nullptr, nullptr };
static IStream*        g_sprPlusStream[3]  = { nullptr, nullptr, nullptr };
static Gdiplus::Image* g_sprTrash[3]  = { nullptr, nullptr, nullptr };
static IStream*        g_sprTrashStream[3] = { nullptr, nullptr, nullptr };

// Dropdown de branches.
static HWND g_hComboBranch = nullptr;
static std::vector<std::wstring> g_remoteBranches;
static std::atomic<bool> g_branchLookupRunning{ false };
static std::wstring g_branchLookupUrl;

// Texto de rodape (placeholder - edite FOOTER_TEXT para mudar o conteudo).
static HWND g_hwndFooter = nullptr;
static constexpr int FOOTER_X = 246;
static constexpr int FOOTER_Y_OFFSET = -28;
static constexpr wchar_t FOOTER_TEXT[] = L"V1.0.1 - 17/08/2026";
static constexpr COLORREF FOOTER_TEXT_COLOR = RGB(0, 182, 255);

// Estado hover/click para owner-draw do botao + e lixeira.
static int g_plusButtonState = 0;
static int g_trashHoverIndex = -1;
static int g_trashPressedIndex = -1;

// Branch selecionado pelo usuario no dropdown (setado em DoPush, lido pela thread).
static std::wstring g_selectedBranch;

static const wchar_t* CONFIG_FILE_NAME = L"config.ini";
static std::atomic<bool> g_isPushing{ false };
static std::atomic<bool> g_lastPushSuccess{ false };

// Limiar para rastrear arquivos com LFS (50 MB - GitHub rejeita >100 MB)
static constexpr long long LFS_SIZE_THRESHOLD = 50LL * 1024LL * 1024LL;

static HMENU CtrlId(int id) { return reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)); }

// ---------------------------------------------------------------------------
//  Forward declarations (definições aparecem mais abaixo)
// ---------------------------------------------------------------------------
// Valida uma URL de repositório Git (https://.../...git ou git@host:user/repo.git).
static bool IsValidGitUrl(const std::wstring& url);
// Fallback GDI para desenhar a lixeira quando o sprite PNG não estiver carregado.
static void DrawTrashIcon(HDC hdc, const RECT& area, COLORREF bg, COLORREF fg);

// ---------------------------------------------------------------------------
//  Perfis de repositório (barra lateral "Meus repos")
// ---------------------------------------------------------------------------
struct RepoProfile
{
    std::wstring name;
    std::wstring folder;
    std::wstring repoUrl;
    std::wstring token;
};

static std::vector<RepoProfile> g_profiles;
static int g_selectedProfile = -1; // índice em g_profiles, -1 = nenhum selecionado

// ---------------------------------------------------------------------------
//  Utilitários de texto
// ---------------------------------------------------------------------------

static std::wstring GetWindowTextStr(HWND hwnd)
{
    const int len = GetWindowTextLengthW(hwnd);
    if (len <= 0) return L"";
    std::vector<wchar_t> buf(static_cast<size_t>(len) + 1, L'\0');
    GetWindowTextW(hwnd, buf.data(), len + 1);
    return std::wstring(buf.data());
}

static void AppendLog(const std::wstring& text)
{
    const int len = GetWindowTextLengthW(g_hEditLog);
    SendMessageW(g_hEditLog, EM_SETSEL, static_cast<WPARAM>(len), static_cast<LPARAM>(len));
    // Insert separator BEFORE the text (except for the very first message),
    // so the last appended message has no trailing newline and the caret
    // sits at the end of the last visible line (not on an empty line below).
    std::wstring toInsert;
    if (len > 0) toInsert += L"\r\n";
    toInsert += text;
    SendMessageW(g_hEditLog, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(toInsert.c_str()));
    SendMessageW(g_hEditLog, EM_SCROLLCARET, 0, 0);
}

static std::string WideToUtf8(const std::wstring& w)
{
    if (w.empty()) return std::string();
    const int size = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
        nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
        out.data(), size, nullptr, nullptr);
    return out;
}

static std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty()) return std::wstring();
    const int size = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), size);
    return out;
}

// ---------------------------------------------------------------------------
//  Ofuscação simples do token (XOR + Base64) - ver aviso no topo
// ---------------------------------------------------------------------------

static const char OBFUSCATION_KEY[] = "TinyBoxGitPusherKey!";

static std::string XorWithKey(const std::string& data)
{
    std::string out = data;
    const size_t keyLen = sizeof(OBFUSCATION_KEY) - 1;
    for (size_t i = 0; i < out.size(); i++)
        out[i] = static_cast<char>(out[i] ^ OBFUSCATION_KEY[i % keyLen]);
    return out;
}

static std::string Base64Encode(const std::string& in)
{
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0, valb = -6;
    for (unsigned char c : in)
    {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0)
        {
            out.push_back(tbl[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6) out.push_back(tbl[((val << 8) >> (valb + 8)) & 0x3F]);
    while (out.size() % 4) out.push_back('=');
    return out;
}

static std::string Base64Decode(const std::string& in)
{
    static int T[256];
    static bool init = false;
    if (!init)
    {
        for (int i = 0; i < 256; i++) T[i] = -1;
        const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; i++) T[static_cast<unsigned char>(tbl[i])] = i;
        init = true;
    }
    std::string out;
    int val = 0, valb = -8;
    for (unsigned char c : in)
    {
        if (T[c] == -1) break;
        val = (val << 6) + T[c];
        valb += 6;
        if (valb >= 0)
        {
            out.push_back(static_cast<char>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

static std::wstring ObfuscateToken(const std::wstring& token)
{
    const std::string utf8 = WideToUtf8(token);
    const std::string xored = XorWithKey(utf8);
    const std::string b64 = Base64Encode(xored);
    return Utf8ToWide(b64);
}

static std::wstring DeobfuscateToken(const std::wstring& stored)
{
    const std::string b64 = WideToUtf8(stored);
    const std::string xored = Base64Decode(b64);
    const std::string original = XorWithKey(xored);
    return Utf8ToWide(original);
}

// ---------------------------------------------------------------------------
//  Config (config.ini ao lado do executável) - agora com múltiplos perfis
// ---------------------------------------------------------------------------

static std::wstring GetConfigPath()
{
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    const std::wstring path(exePath);
    const size_t pos = path.find_last_of(L"\\/");
    const std::wstring dir = (pos != std::wstring::npos) ? path.substr(0, pos + 1) : L"";
    return dir + CONFIG_FILE_NAME;
}

// Remove caracteres que atrapalhariam o nome da seção do INI ou o separador '|'
static std::wstring SanitizeProfileName(const std::wstring& raw)
{
    std::wstring out;
    for (wchar_t c : raw)
    {
        if (c == L'|' || c == L'[' || c == L']' || c == L'\r' || c == L'\n') continue;
        out.push_back(c);
    }
    // Aparência: remove espaços nas pontas
    while (!out.empty() && out.front() == L' ') out.erase(out.begin());
    while (!out.empty() && out.back() == L' ') out.pop_back();
    return out.empty() ? L"Repositorio" : out;
}

// Garante que o nome não colide com um já existente na lista (anexa número se precisar)
static std::wstring MakeUniqueProfileName(const std::wstring& base)
{
    std::wstring candidate = base;
    int suffix = 2;
    bool collided;
    do
    {
        collided = false;
        for (const auto& p : g_profiles)
        {
            if (p.name == candidate) { collided = true; break; }
        }
        if (collided)
        {
            candidate = base + L" (" + std::to_wstring(suffix) + L")";
            suffix++;
        }
    } while (collided);
    return candidate;
}

static void SaveProfilesConfig()
{
    const std::wstring path = GetConfigPath();

    // Limpa seções de perfis antigos escrevendo um arquivo novo do zero e
    // preservando apenas a seção [EasyGitPusher] com a lista atual + selecionado.
    // (WritePrivateProfileString com seção inteira não remove seções antigas
    // automaticamente, então vamos escrever um arquivo novo por completo.)
    DeleteFileW(path.c_str());

    std::wstring joined;
    for (size_t i = 0; i < g_profiles.size(); i++)
    {
        joined += g_profiles[i].name;
        if (i + 1 < g_profiles.size()) joined += L"|";
    }

    WritePrivateProfileStringW(L"EasyGitPusher", L"Profiles", joined.c_str(), path.c_str());

    const std::wstring selectedName =
        (g_selectedProfile >= 0 && g_selectedProfile < (int)g_profiles.size())
        ? g_profiles[g_selectedProfile].name : L"";
    WritePrivateProfileStringW(L"EasyGitPusher", L"Selected", selectedName.c_str(), path.c_str());

    for (const auto& p : g_profiles)
    {
        const std::wstring section = L"Profile:" + p.name;
        WritePrivateProfileStringW(section.c_str(), L"Folder", p.folder.c_str(), path.c_str());
        WritePrivateProfileStringW(section.c_str(), L"RepoUrl", p.repoUrl.c_str(), path.c_str());
        const std::wstring obf = ObfuscateToken(p.token);
        WritePrivateProfileStringW(section.c_str(), L"Token", obf.c_str(), path.c_str());
    }
}

static void LoadProfilesConfig()
{
    g_profiles.clear();
    g_selectedProfile = -1;

    const std::wstring path = GetConfigPath();
    constexpr size_t BUF_SIZE = 8192;
    auto bufPtr = std::make_unique<wchar_t[]>(BUF_SIZE);
    wchar_t* buf = bufPtr.get();
    buf[0] = L'\0';

    GetPrivateProfileStringW(L"EasyGitPusher", L"Profiles", L"", buf, static_cast<DWORD>(BUF_SIZE), path.c_str());
    std::wstring joined = buf;

    std::vector<std::wstring> names;
    size_t start = 0;
    while (start <= joined.size())
    {
        size_t sep = joined.find(L'|', start);
        if (sep == std::wstring::npos)
        {
            if (start < joined.size()) names.push_back(joined.substr(start));
            break;
        }
        if (sep > start) names.push_back(joined.substr(start, sep - start));
        start = sep + 1;
    }

    for (const auto& name : names)
    {
        if (name.empty()) continue;
        RepoProfile p;
        p.name = name;

        const std::wstring section = L"Profile:" + name;

        buf[0] = L'\0';
        GetPrivateProfileStringW(section.c_str(), L"Folder", L"", buf, static_cast<DWORD>(BUF_SIZE), path.c_str());
        p.folder = buf;

        buf[0] = L'\0';
        GetPrivateProfileStringW(section.c_str(), L"RepoUrl", L"", buf, static_cast<DWORD>(BUF_SIZE), path.c_str());
        p.repoUrl = buf;

        buf[0] = L'\0';
        GetPrivateProfileStringW(section.c_str(), L"Token", L"", buf, static_cast<DWORD>(BUF_SIZE), path.c_str());
        const std::wstring obf = buf;
        p.token = obf.empty() ? L"" : DeobfuscateToken(obf);

        g_profiles.push_back(p);
    }

    buf[0] = L'\0';
    GetPrivateProfileStringW(L"EasyGitPusher", L"Selected", L"", buf, static_cast<DWORD>(BUF_SIZE), path.c_str());
    const std::wstring selectedName = buf;
    if (!selectedName.empty())
    {
        for (size_t i = 0; i < g_profiles.size(); i++)
        {
            if (g_profiles[i].name == selectedName) { g_selectedProfile = (int)i; break; }
        }
    }
}

// ---------------------------------------------------------------------------
//  Executa um comando externo (via cmd.exe) capturando stdout+stderr
// ---------------------------------------------------------------------------

static bool RunCommand(const std::wstring& command, const std::wstring& workingDir, std::wstring& outCombined)
{
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE hReadPipe = nullptr;
    HANDLE hWritePipe = nullptr;
    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0))
        return false;

    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags |= STARTF_USESTDHANDLES;
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;
    si.hStdInput = nullptr;

    PROCESS_INFORMATION pi{};

    std::wstring fullCmd = L"cmd.exe /C " + command;
    std::vector<wchar_t> cmdBuf(fullCmd.begin(), fullCmd.end());
    cmdBuf.push_back(0);

    const BOOL ok = CreateProcessW(
        nullptr, cmdBuf.data(),
        nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        workingDir.empty() ? nullptr : workingDir.c_str(),
        &si, &pi
    );

    CloseHandle(hWritePipe);

    if (!ok)
    {
        CloseHandle(hReadPipe);
        outCombined = L"[Falha ao iniciar o processo]";
        return false;
    }

    std::string rawOutput;
    char readBuf[4096]{};
    DWORD bytesRead = 0;
    while (ReadFile(hReadPipe, readBuf, sizeof(readBuf) - 1, &bytesRead, nullptr) && bytesRead > 0)
    {
        readBuf[bytesRead] = '\0';
        rawOutput.append(readBuf, bytesRead);
    }

    WaitForSingleObject(pi.hProcess, INFINITE);

    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);

    CloseHandle(hReadPipe);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    outCombined = Utf8ToWide(rawOutput);
    return exitCode == 0;
}

// ---------------------------------------------------------------------------
//  Monta a URL do repo com token embutido para autenticação
// ---------------------------------------------------------------------------

static std::wstring BuildAuthenticatedUrl(const std::wstring& repoUrl, const std::wstring& token)
{
    const std::wstring httpsPrefix = L"https://";
    if (repoUrl.rfind(httpsPrefix, 0) == 0 && !token.empty())
    {
        const std::wstring rest = repoUrl.substr(httpsPrefix.size());
        return httpsPrefix + token + L"@" + rest;
    }
    return repoUrl;
}

// Extrai um nome curto a partir da URL do repositório (ex: "TinyBox-OS")
static std::wstring DeriveNameFromRepoUrl(const std::wstring& repoUrl)
{
    std::wstring url = repoUrl;
    while (!url.empty() && (url.back() == L'/' || url.back() == L'\\')) url.pop_back();

    size_t slash = url.find_last_of(L"/\\");
    std::wstring last = (slash != std::wstring::npos) ? url.substr(slash + 1) : url;

    const std::wstring gitSuffix = L".git";
    if (last.size() > gitSuffix.size() &&
        last.compare(last.size() - gitSuffix.size(), gitSuffix.size(), gitSuffix) == 0)
    {
        last = last.substr(0, last.size() - gitSuffix.size());
    }
    return last;
}

// Extrai um nome curto a partir do caminho da pasta (último segmento)
static std::wstring DeriveNameFromFolder(const std::wstring& folder)
{
    std::wstring path = folder;
    while (!path.empty() && (path.back() == L'/' || path.back() == L'\\')) path.pop_back();
    size_t slash = path.find_last_of(L"/\\");
    return (slash != std::wstring::npos) ? path.substr(slash + 1) : path;
}

// ---------------------------------------------------------------------------
//  Git LFS - detecção e configuração automática de arquivos grandes
// ---------------------------------------------------------------------------

static void FindLargeFilesRecursive(const std::wstring& basePath,
    const std::wstring& currentPath,
    std::vector<std::wstring>& largeFiles)
{
    const std::wstring searchPath = currentPath + L"\\*";
    auto fd = std::make_unique<WIN32_FIND_DATAW>();
    const HANDLE hFind = FindFirstFileW(searchPath.c_str(), fd.get());
    if (hFind == INVALID_HANDLE_VALUE) return;

    do
    {
        const std::wstring fileName = fd->cFileName;
        if (fileName == L"." || fileName == L"..") continue;

        const std::wstring fullPath = currentPath + L"\\" + fileName;

        if (fd->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            if (fileName == L".git" || fileName == L"node_modules") continue;
            FindLargeFilesRecursive(basePath, fullPath, largeFiles);
        }
        else
        {
            LARGE_INTEGER fileSize{};
            fileSize.LowPart = fd->nFileSizeLow;
            fileSize.HighPart = fd->nFileSizeHigh;

            if (fileSize.QuadPart > LFS_SIZE_THRESHOLD)
            {
                std::wstring relPath = fullPath.substr(basePath.size());
                if (!relPath.empty() && (relPath[0] == L'\\' || relPath[0] == L'/'))
                    relPath = relPath.substr(1);
                for (wchar_t& c : relPath)
                    if (c == L'\\') c = L'/';
                largeFiles.push_back(relPath);
            }
        }
    } while (FindNextFileW(hFind, fd.get()));

    FindClose(hFind);
}

static bool SetupLfs(const std::wstring& folder)
{
    std::wstring output{};

    if (!RunCommand(L"git lfs --version", folder, output))
    {
        AppendLog(L"[LFS] git-lfs não encontrado no PATH.");
        AppendLog(L"[LFS] Se você tiver arquivos >100MB, instale em:");
        AppendLog(L"[LFS]   https://git-lfs.github.com/");
        return false;
    }

    AppendLog(L"[LFS] git-lfs detectado: " + output);

    RunCommand(L"git lfs install --local", folder, output);
    AppendLog(output);

    std::vector<std::wstring> largeFiles;
    FindLargeFilesRecursive(folder, folder, largeFiles);

    if (largeFiles.empty())
    {
        AppendLog(L"[LFS] Nenhum arquivo >50MB encontrado. Nada a rastrear.");
        return false;
    }

    for (const auto& relPath : largeFiles)
    {
        AppendLog(L"[LFS] Rastreando arquivo grande (" + relPath + L")...");
        std::wstring trackOut{};
        RunCommand(L"git lfs track \"" + relPath + L"\"", folder, trackOut);
        if (!trackOut.empty())
            AppendLog(L"  " + trackOut);
    }

    std::wstring addAttrOut{};
    RunCommand(L"git -c core.safecrlf=false add .gitattributes", folder, addAttrOut);

    for (const auto& relPath : largeFiles)
    {
        std::wstring rmOut{}, addOut{};
        RunCommand(L"git rm --cached \"" + relPath + L"\"", folder, rmOut);
        RunCommand(L"git add --renormalize \"" + relPath + L"\"", folder, addOut);
    }

    AppendLog(L"[LFS] " + std::to_wstring(largeFiles.size()) +
        L" arquivo(s) convertido(s) para pointer LFS no stage.");
    return true;
}

// ---------------------------------------------------------------------------
//  Lógica principal do Push
// ---------------------------------------------------------------------------

struct PushParams
{
    std::wstring folder;
    std::wstring repo;
    std::wstring token;
    std::wstring commitMsg;
};

static void DoPushWorker(const std::wstring& folder, const std::wstring& repo,
    const std::wstring& token, const std::wstring& commitMsgIn)
{
    const std::wstring commitMsg = commitMsgIn.empty() ? L"Update via EasyGitPusher" : commitMsgIn;

    SetWindowTextW(g_hEditLog, L"");
    AppendLog(L"===== Iniciando push =====");
    AppendLog(L"Pasta: " + folder);
    AppendLog(L"Repositório: " + repo);
    AppendLog(L"");

    const std::wstring authUrl = BuildAuthenticatedUrl(repo, token);
    std::wstring output{};
    bool finalSuccess = false;

    do
    {
        const std::wstring gitDir = folder + L"\\.git";
        const DWORD attrs = GetFileAttributesW(gitDir.c_str());
        const bool hasGit = (attrs != INVALID_FILE_ATTRIBUTES) && (attrs & FILE_ATTRIBUTE_DIRECTORY);

        if (!hasGit)
        {
            AppendLog(L"[1/7] Inicializando repositório git...");
            RunCommand(L"git init", folder, output);
            AppendLog(output);
        }
        else
        {
            AppendLog(L"[1/7] Repositório git já existe, pulando init.");
        }

        AppendLog(L"[2/7] Configurando remote 'origin'...");
        RunCommand(L"git remote remove origin", folder, output);
        const bool remoteOk = RunCommand(L"git remote add origin \"" + authUrl + L"\"", folder, output);
        AppendLog(output);
        if (!remoteOk)
        {
            AppendLog(L"[ERRO] Não foi possível configurar o remote. Abortando.");
            break;
        }

        AppendLog(L"[3/7] Adicionando arquivos (git add -A)...");
        RunCommand(L"git -c core.safecrlf=false add -A", folder, output);
        AppendLog(output);

        AppendLog(L"[4/7] Verificando arquivos grandes (Git LFS)...");
        SetupLfs(folder);

        AppendLog(L"[5/7] Criando commit...");
        const bool committed = RunCommand(L"git commit -m \"" + commitMsg + L"\"", folder, output);
        AppendLog(output);
        if (!committed)
            AppendLog(L"(Sem alterações novas para commitar - seguindo para o push mesmo assim)");

        // Branch = selecionado no dropdown (validado em DoPush antes da thread).
        AppendLog(L"[6/7] Sincronizando branch (" + g_selectedBranch + L")...");
        std::wstring branchOutput{};
        RunCommand(L"git rev-parse --abbrev-ref HEAD", folder, branchOutput);
        std::wstring localBranch = branchOutput;
        while (!localBranch.empty() && (localBranch.back() == L'\n' || localBranch.back() == L'\r'))
            localBranch.pop_back();

        std::wstring branch = g_selectedBranch;
        if (localBranch.empty() || localBranch == L"HEAD")
        {
            RunCommand(L"git checkout -B " + branch, folder, output);
            AppendLog(output);
        }
        else if (localBranch != branch)
        {
            AppendLog(L"Renomeando branch local '" + localBranch + L"' para '" + branch + L"'...");
            RunCommand(L"git branch -M " + branch, folder, output);
            AppendLog(output);
        }
        else
        {
            AppendLog(L"Branch atual: " + branch);
        }

        AppendLog(L"[7/7] Enviando para o GitHub (git push)...");
        bool pushOk = RunCommand(L"git push -u origin " + branch, folder, output);
        AppendLog(output);

        // Bruteforce: se o push foi rejeitado porque o remote tem commits que
        // nao temos localmente, faz git pull --rebase para integrar as mudancas
        // remotas (se possivel sem conflito) e em seguida FORCE-PUSH para
        // garantir que o conteudo local sobrescreva o remote. Se o rebase
        // encontrar conflitos, aborta o rebase e faz mesmo assim o force-push
        // (o conteudo local vence).
        if (!pushOk && (output.find(L"rejected") != std::wstring::npos ||
                        output.find(L"fetch first") != std::wstring::npos ||
                        output.find(L"non-fast-forward") != std::wstring::npos))
        {
            AppendLog(L"");
            AppendLog(L"[7.5/7] Push rejeitado pelo remote. Tentando git pull --rebase");
            AppendLog(L"         para integrar mudancas remotas antes do bruteforce push...");

            const std::wstring pullCmd = L"git pull --rebase origin " + branch;
            std::wstring pullOutput;
            const bool pullOk = RunCommand(pullCmd, folder, pullOutput);
            AppendLog(pullOutput);

            if (pullOk) {
                AppendLog(L"Pull --rebase OK. Forcando push (git push --force)...");
                AppendLog(L"");
                output.clear();
                pushOk = RunCommand(L"git push --force -u origin " + branch, folder, output);
                AppendLog(output);
            } else {
                AppendLog(L"[AVISO] git pull --rebase encontrou conflitos. Abortando rebase");
                AppendLog(L"         e fazendo bruteforce push (conteudo local sobrescreve");
                AppendLog(L"         o remote - qualquer mudanca remota conflitante sera perdida)...");
                std::wstring abortOutput;
                RunCommand(L"git rebase --abort", folder, abortOutput);
                AppendLog(abortOutput);
                AppendLog(L"");
                AppendLog(L"[7.6/7] Forcando push (git push --force)...");
                output.clear();
                pushOk = RunCommand(L"git push --force -u origin " + branch, folder, output);
                AppendLog(output);
            }
        }

        if (pushOk)
        {
            AppendLog(L"===== Push concluido com sucesso! =====");
            finalSuccess = true;
        }
        else
        {
            AppendLog(L"===== Push falhou. Veja os detalhes acima. =====");
        }

    } while (false);

    g_lastPushSuccess = finalSuccess;
    PostMessageW(g_hMainWnd, WM_PUSH_DONE, 0, 0);
}

static unsigned __stdcall PushThreadProc(void* rawParam)
{
    PushParams* p = static_cast<PushParams*>(rawParam);
    DoPushWorker(p->folder, p->repo, p->token, p->commitMsg);
    delete p;
    return 0;
}

static void DoPush()
{
    const std::wstring repoUrl = GetWindowTextStr(g_hEditRepo);
    if (!IsValidGitUrl(repoUrl)) {
        MessageBoxW(g_hMainWnd, L"Link de repositório inválido.\n"
                            L"Use https://github.com/usuario/repo.git ou git@github.com:usuario/repo.git",
                  L"Erro", MB_OK | MB_ICONERROR);
        return;
    }
    const int branchSel = static_cast<int>(SendMessageW(g_hComboBranch, CB_GETCURSEL, 0, 0));
    std::wstring selectedBranch;
    if (branchSel != CB_ERR && branchSel >= 0 && branchSel < (int)g_remoteBranches.size()) {
        selectedBranch = g_remoteBranches[branchSel];
    }
    if (selectedBranch.empty()) {
        MessageBoxW(g_hMainWnd, L"Selecione um branch antes de enviar.\n"
                            L"Se o dropdown está vazio, aguarde o lookup automático terminar\n"
                            L"ou verifique se o link está correto.",
                  L"Erro", MB_OK | MB_ICONWARNING);
        return;
    }
    g_selectedBranch = selectedBranch;

    if (g_isPushing) return;

    const std::wstring folder = GetWindowTextStr(g_hEditFolder);
    const std::wstring repo = GetWindowTextStr(g_hEditRepo);
    const std::wstring token = GetWindowTextStr(g_hEditToken);
    const std::wstring commitMsg = GetWindowTextStr(g_hEditCommitMsg);

    if (folder.empty() || repo.empty() || token.empty())
    {
        MessageBoxW(g_hMainWnd,
            L"Preencha a pasta, o link do repositório e o token antes de continuar.",
            L"Campos incompletos", MB_OK | MB_ICONWARNING);
        return;
    }

    // Se há um repositório selecionado na barra lateral, mantém ele atualizado
    // com o que estiver nos campos no momento do push.
    if (g_selectedProfile >= 0 && g_selectedProfile < (int)g_profiles.size())
    {
        g_profiles[g_selectedProfile].folder = folder;
        g_profiles[g_selectedProfile].repoUrl = repo;
        g_profiles[g_selectedProfile].token = token;
        SaveProfilesConfig();
    }

    g_isPushing = true;
    EnableWindow(GetDlgItem(g_hMainWnd, ID_BTN_PUSH), FALSE);
    SetWindowTextW(GetDlgItem(g_hMainWnd, ID_BTN_PUSH), L"Enviando...");

    PushParams* const params = new PushParams{ folder, repo, token, commitMsg };
    const uintptr_t handle = _beginthreadex(nullptr, 0, PushThreadProc, params, 0, nullptr);

    if (handle == 0)
    {
        delete params;
        g_isPushing = false;
        EnableWindow(GetDlgItem(g_hMainWnd, ID_BTN_PUSH), TRUE);
        SetWindowTextW(GetDlgItem(g_hMainWnd, ID_BTN_PUSH), L"PUSH");
        MessageBoxW(g_hMainWnd, L"Não foi possível iniciar o processo de push.",
            L"Erro", MB_OK | MB_ICONERROR);
    }
    else
    {
        CloseHandle(reinterpret_cast<HANDLE>(handle));
    }
}

// ---------------------------------------------------------------------------
//  Seleção de pasta (IFileOpenDialog - API moderna)
// ---------------------------------------------------------------------------

static void BrowseForFolder()
{
    IFileOpenDialog* pDialog = nullptr;
    const HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pDialog));

    if (FAILED(hr))
    {
        MessageBoxW(g_hMainWnd, L"Não foi possível abrir o seletor de pastas.",
            L"Erro", MB_OK | MB_ICONERROR);
        return;
    }

    DWORD options = 0;
    pDialog->GetOptions(&options);
    pDialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    pDialog->SetTitle(L"Selecione a pasta a ser enviada para o GitHub");

    const HRESULT showHr = pDialog->Show(g_hMainWnd);
    if (SUCCEEDED(showHr))
    {
        IShellItem* pItem = nullptr;
        if (SUCCEEDED(pDialog->GetResult(&pItem)))
        {
            PWSTR path = nullptr;
            if (SUCCEEDED(pItem->GetDisplayName(SIGDN_FILESYSPATH, &path)))
            {
                SetWindowTextW(g_hEditFolder, path);
                CoTaskMemFree(path);
            }
            pItem->Release();
        }
    }

    pDialog->Release();
}

// ---------------------------------------------------------------------------
//  Barra lateral "Meus repos"
// ---------------------------------------------------------------------------

static void RebuildRepoList()
{
    SendMessageW(g_hListRepos, LB_RESETCONTENT, 0, 0);
    for (const auto& p : g_profiles)
        SendMessageW(g_hListRepos, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(p.name.c_str()));

    if (g_selectedProfile >= 0 && g_selectedProfile < (int)g_profiles.size())
        SendMessageW(g_hListRepos, LB_SETCURSEL, static_cast<WPARAM>(g_selectedProfile), 0);
    else
        SendMessageW(g_hListRepos, LB_SETCURSEL, static_cast<WPARAM>(-1), 0);

    InvalidateRect(g_hListRepos, nullptr, TRUE);
}

// Preenche os campos principais a partir do perfil selecionado
static void LoadProfileIntoFields(int index)
{
    if (index < 0 || index >= (int)g_profiles.size()) return;
    const RepoProfile& p = g_profiles[index];
    SetWindowTextW(g_hEditFolder, p.folder.c_str());
    SetWindowTextW(g_hEditRepo, p.repoUrl.c_str());
    SetWindowTextW(g_hEditToken, p.token.c_str());
}

// Adiciona a configuração atual dos campos como um novo repositório na lista
static void AddCurrentAsProfile()
{
    const std::wstring folder = GetWindowTextStr(g_hEditFolder);
    const std::wstring repo = GetWindowTextStr(g_hEditRepo);
    const std::wstring token = GetWindowTextStr(g_hEditToken);

    if (folder.empty() && repo.empty())
    {
        MessageBoxW(g_hMainWnd,
            L"Preencha ao menos a pasta ou o link do repositório antes de adicionar a lista.",
            L"Campos vazios", MB_OK | MB_ICONWARNING);
        return;
    }

    std::wstring baseName = !repo.empty() ? DeriveNameFromRepoUrl(repo) : DeriveNameFromFolder(folder);
    baseName = SanitizeProfileName(baseName);
    const std::wstring finalName = MakeUniqueProfileName(baseName);

    RepoProfile p;
    p.name = finalName;
    p.folder = folder;
    p.repoUrl = repo;
    p.token = token;

    g_profiles.push_back(p);
    g_selectedProfile = (int)g_profiles.size() - 1;

    SaveProfilesConfig();
    RebuildRepoList();
}

// Remove um repositório da lista (não mexe no GitHub nem no disco)
static void DeleteProfileAt(int index)
{
    if (index < 0 || index >= (int)g_profiles.size()) return;

    const std::wstring msg = L"Remover \"" + g_profiles[index].name +
        L"\" da lista de repositórios?\n\n(Isso não apaga nada no GitHub nem na sua pasta local, só tira da lista aqui.)";
    const int choice = MessageBoxW(g_hMainWnd, msg.c_str(), L"Remover repositório", MB_YESNO | MB_ICONQUESTION);
    if (choice != IDYES) return;

    g_profiles.erase(g_profiles.begin() + index);

    if (g_selectedProfile == index)
    {
        g_selectedProfile = -1;
    }
    else if (g_selectedProfile > index)
    {
        g_selectedProfile -= 1;
    }

    SaveProfilesConfig();
    RebuildRepoList();
}

// Calcula o retângulo da área clicável da lixeira dentro de um item da lista
static RECT GetTrashRectForItem(const RECT& itemRect)
{
    RECT trash = itemRect;
    trash.left = itemRect.right - TRASH_ICON_W;
    return trash;
}

// Desenha o sprite da lixeira (3 estados). Se nao carregado, cai para DrawTrashIcon fallback.
static void DrawTrashSprite(HDC hdc, const RECT& area, int stateIndex)
{
    if (stateIndex < 0 || stateIndex > 2) stateIndex = 0;
    if (g_sprTrash[stateIndex]) {
        Gdiplus::Graphics graphics(hdc);
        graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        const int imgW = static_cast<int>(g_sprTrash[stateIndex]->GetWidth());
        const int imgH = static_cast<int>(g_sprTrash[stateIndex]->GetHeight());
        const int areaW = area.right - area.left;
        const int areaH = area.bottom - area.top;
        int drawW = imgW, drawH = imgH;
        if (imgW > 0 && imgH > 0 && areaW > 0 && areaH > 0) {
            const float sx = static_cast<float>(areaW) / imgW;
            const float sy = static_cast<float>(areaH) / imgH;
            const float s = (sx < sy) ? sx : sy;
            drawW = static_cast<int>(imgW * s);
            drawH = static_cast<int>(imgH * s);
        }
        const int x = area.left + (areaW - drawW) / 2;
        const int y = area.top  + (areaH - drawH) / 2;
        graphics.DrawImage(g_sprTrash[stateIndex], x, y, drawW, drawH);
        return;
    }
    DrawTrashIcon(hdc, area, RGB(220, 53, 69), RGB(255, 255, 255));
}

// [Fallback] Desenha um ícone de lixeira simples via GDI dentro do retângulo dado
static void DrawTrashIcon(HDC hdc, const RECT& area, COLORREF bg, COLORREF fg)
{
    const int cx = (area.left + area.right) / 2;
    const int cy = (area.top + area.bottom) / 2;
    const int half = 8;

    HBRUSH hBrushBg = CreateSolidBrush(bg);
    HPEN hPenBg = CreatePen(PS_SOLID, 1, bg);
    HGDIOBJ oldBrush = SelectObject(hdc, hBrushBg);
    HGDIOBJ oldPen = SelectObject(hdc, hPenBg);
    RoundRect(hdc, cx - 13, cy - 13, cx + 13, cy + 13, 8, 8);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(hBrushBg);
    DeleteObject(hPenBg);

    HPEN hPenFg = CreatePen(PS_SOLID, 2, fg);
    HGDIOBJ oldPen2 = SelectObject(hdc, hPenFg);

    // Corpo da lixeira
    MoveToEx(hdc, cx - half + 2, cy - half + 4, nullptr); LineTo(hdc, cx - half + 2, cy + half - 2);
    MoveToEx(hdc, cx - half + 2, cy + half - 2, nullptr); LineTo(hdc, cx + half - 2, cy + half - 2);
    MoveToEx(hdc, cx + half - 2, cy + half - 2, nullptr); LineTo(hdc, cx + half - 2, cy - half + 4);

    // Tampa
    MoveToEx(hdc, cx - half - 1, cy - half + 4, nullptr); LineTo(hdc, cx + half + 1, cy - half + 4);
    // Alça
    MoveToEx(hdc, cx - 4, cy - half + 4, nullptr); LineTo(hdc, cx - 4, cy - half);
    MoveToEx(hdc, cx - 4, cy - half, nullptr); LineTo(hdc, cx + 4, cy - half);
    MoveToEx(hdc, cx + 4, cy - half, nullptr); LineTo(hdc, cx + 4, cy - half + 4);

    // Riscos internos
    MoveToEx(hdc, cx - 3, cy - half + 7, nullptr); LineTo(hdc, cx - 3, cy + half - 5);
    MoveToEx(hdc, cx + 3, cy - half + 7, nullptr); LineTo(hdc, cx + 3, cy + half - 5);

    SelectObject(hdc, oldPen2);
    DeleteObject(hPenFg);
}

// Desenho customizado de cada item da lista de repositórios
static void OnDrawRepoItem(LPDRAWITEMSTRUCT dis)
{
    if (dis->itemID == (UINT)-1) return;

    const bool selected = (dis->itemState & ODS_SELECTED) != 0;
    const COLORREF bgColor = selected ? RGB(0, 120, 170) : RGB(0x21, 0x21, 0x21);
    const COLORREF textColor = RGB(240, 240, 240);

    HBRUSH hBrush = CreateSolidBrush(bgColor);
    FillRect(dis->hDC, &dis->rcItem, hBrush);
    DeleteObject(hBrush);

    if (dis->itemID < g_profiles.size())
    {
        const std::wstring& name = g_profiles[dis->itemID].name;

        RECT textRect = dis->rcItem;
        textRect.left += 10;
        textRect.right -= TRASH_ICON_W;

        SetBkMode(dis->hDC, TRANSPARENT);
        SetTextColor(dis->hDC, textColor);
        HGDIOBJ oldFont = SelectObject(dis->hDC, g_hFontBold);
        DrawTextW(dis->hDC, name.c_str(), -1, &textRect, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS);
        SelectObject(dis->hDC, oldFont);

        const RECT trashRect = GetTrashRectForItem(dis->rcItem);
        int trashState = 0;
        if ((int)dis->itemID == g_trashHoverIndex)    trashState = 1;
        if ((int)dis->itemID == g_trashPressedIndex) trashState = 2;
        DrawTrashSprite(dis->hDC, trashRect, trashState);
    }

    if (selected)
    {
        RECT frame = dis->rcItem;
        HPEN hPen = CreatePen(PS_SOLID, 1, RGB(0, 150, 180));
        HGDIOBJ oldPen = SelectObject(dis->hDC, hPen);
        HGDIOBJ oldBrush = SelectObject(dis->hDC, GetStockObject(NULL_BRUSH));
        Rectangle(dis->hDC, frame.left, frame.top, frame.right, frame.bottom);
        SelectObject(dis->hDC, oldPen);
        SelectObject(dis->hDC, oldBrush);
        DeleteObject(hPen);
    }
}

// Subclass da listbox: intercepta cliques na área da lixeira antes da
// seleção padrão ser processada, para não "selecionar e carregar" um
// item que na verdade o usuário queria deletar.
static int HitTestTrash(HWND hwnd, int x, int y)
{
    BOOL outside = FALSE;
    const LRESULT itemFromPoint = SendMessageW(hwnd, LB_ITEMFROMPOINT, 0, MAKELPARAM(x, y));
    const int index = LOWORD(itemFromPoint);
    outside = HIWORD(itemFromPoint) != 0;
    if (outside || index < 0 || index >= (int)g_profiles.size()) return -1;
    RECT itemRect{};
    SendMessageW(hwnd, LB_GETITEMRECT, static_cast<WPARAM>(index), reinterpret_cast<LPARAM>(&itemRect));
    const RECT trashRect = GetTrashRectForItem(itemRect);
    if (x >= trashRect.left && x <= trashRect.right && y >= trashRect.top && y <= trashRect.bottom)
        return index;
    return -1;
}

// Subclass do botao "+": rastreia hover/pressed para animacao de 3 estados.
// BS_OWNERDRAW nativamente so recebe ODS_SELECTED (pressionado) - nunca
// ODS_HOTLIGHT. Por isso trackeamos manualmente via WM_MOUSEMOVE e
// WM_MOUSELEAVE, guardando o estado em g_plusButtonState.
// 0 = normal, 1 = hover, 2 = pressed (botao esquerdo esta apertado).
// A acao em si (BN_CLICKED -> AddCurrentAsProfile) continua disparando
// nativamente no LBUTTONUP do BUTTON control.
static LRESULT CALLBACK AddRepoButtonSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
    UINT_PTR /*uIdSubclass*/, DWORD_PTR /*dwRefData*/)
{
    switch (msg)
    {
    case WM_LBUTTONDOWN:
    {
        g_plusButtonState = 2;
        InvalidateRect(hwnd, nullptr, FALSE);
        // NAO retorna: deixa DefSubclassProc capturar o mouse e disparar
        // BN_CLICKED no LBUTTONUP (comportamento padrao do BUTTON).
        break;
    }
    case WM_LBUTTONUP:
    {
        // Depois do release, ficamos em hover se o mouse ainda esta dentro.
        // DefSubclassProc vai disparar BN_CLICKED -> AddCurrentAsProfile.
        POINT pt { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        RECT rc;
        GetClientRect(hwnd, &rc);
        g_plusButtonState = PtInRect(&rc, pt) ? 1 : 0;
        InvalidateRect(hwnd, nullptr, FALSE);
        break;
    }
    case WM_MOUSEMOVE:
    {
        // Garante WM_MOUSELEAVE quando o cursor sair.
        TRACKMOUSEEVENT tme {};
        tme.cbSize = sizeof(tme);
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = hwnd;
        TrackMouseEvent(&tme);
        // So mudamos para hover (1) se NAO estamos pressionados.
        // Se estamos pressionados, mantemos state=2.
        if (g_plusButtonState != 2) {
            g_plusButtonState = 1;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        break;
    }
    case WM_MOUSELEAVE:
        if (g_plusButtonState != 0) {
            g_plusButtonState = 0;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        break;
    default:
        break;
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

static LRESULT CALLBACK RepoListSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
    UINT_PTR /*uIdSubclass*/, DWORD_PTR /*dwRefData*/)
{
    switch (msg)
    {
    case WM_LBUTTONDOWN:
    {
        const int x = GET_X_LPARAM(lParam);
        const int y = GET_Y_LPARAM(lParam);

        BOOL outside = FALSE;
        const LRESULT itemFromPoint = SendMessageW(hwnd, LB_ITEMFROMPOINT, 0, MAKELPARAM(x, y));
        const int index = LOWORD(itemFromPoint);
        outside = HIWORD(itemFromPoint) != 0;

        if (!outside && index >= 0 && index < (int)g_profiles.size())
        {
            RECT itemRect{};
            SendMessageW(hwnd, LB_GETITEMRECT, static_cast<WPARAM>(index), reinterpret_cast<LPARAM>(&itemRect));
            const RECT trashRect = GetTrashRectForItem(itemRect);

            if (x >= trashRect.left && x <= trashRect.right)
            {
                // Press: marca o item como "clicado" mas NAO deleta ainda.
                // O delete so acontece no LBUTTONUP se o mouse ainda
                // estiver sobre a lixeira do mesmo item. Isso da ao
                // usuario a chance de cancelar arrastando para fora.
                g_trashPressedIndex = index;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0; // consome o clique, não deixa a listbox processar seleção
            }
        }
        // Clique fora da lixeira: limpa estado pressed.
        g_trashPressedIndex = -1;
        break;  // deixa DefSubclassProc tratar o LBUTTONDOWN normalmente (seleção do item)
    }
    case WM_MOUSEMOVE:
    {
        TRACKMOUSEEVENT tme{};
        tme.cbSize = sizeof(tme);
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = hwnd;
        TrackMouseEvent(&tme);
        const int x = GET_X_LPARAM(lParam);
        const int y = GET_Y_LPARAM(lParam);
        const int newHover = HitTestTrash(hwnd, x, y);
        if (newHover != g_trashHoverIndex) {
            g_trashHoverIndex = newHover;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        if (newHover >= 0) SetCursor(LoadCursorW(nullptr, IDC_HAND));
        break;
    }
    case WM_MOUSELEAVE:
        if (g_trashHoverIndex != -1) {
            g_trashHoverIndex = -1;
            g_trashPressedIndex = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        break;
    case WM_LBUTTONUP:
    {
        // Se houver um item marcado como pressed, verifica se o release
        // aconteceu sobre a lixeira DO MESMO item. Se sim, deleta.
        // Caso contrario (mouse foi arrastado para fora), cancela.
        if (g_trashPressedIndex >= 0) {
            const int x = GET_X_LPARAM(lParam);
            const int y = GET_Y_LPARAM(lParam);
            const int releasedOn = HitTestTrash(hwnd, x, y);
            const int pressedIdx = g_trashPressedIndex;
            g_trashPressedIndex = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
            if (releasedOn == pressedIdx && releasedOn >= 0 && releasedOn < (int)g_profiles.size()) {
                DeleteProfileAt(releasedOn);
            }
            return 0; // consome o release se foi sobre trash
        }
        break;
    }
    default:
        break;
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
//  Subclass de EDIT: habilita Ctrl+A para "selecionar tudo"
// ---------------------------------------------------------------------------

// Carrega um PNG embutido no EXE como recurso (tipo "PNG").
// Retorna um Gdiplus::Image* (caller fica dono) ou nullptr se falhar.
// O IStream criado internamente e mantido vivo em *outStream para que
// a imagem continue valida; chame ReleaseLogoStream() no encerramento.
// Valida se uma URL de repositorio Git e aceitavel.
static bool IsValidGitUrl(const std::wstring& url)
{
    if (url.size() < 8) return false;
    if (url.rfind(L"https://", 0) == 0 || url.rfind(L"http://", 0) == 0) {
        int slashes = 0;
        for (wchar_t c : url) if (c == L'/') slashes++;
        return slashes >= 4;
    }
    size_t at = url.find(L'@');
    if (at != std::wstring::npos && at > 0) {
        size_t colon = url.find(L':', at);
        if (colon != std::wstring::npos && colon > at) {
            return url.size() > colon + 3;
        }
    }
    return false;
}

enum class GitUrlType { Https, Ssh, Invalid };
static GitUrlType ClassifyGitUrl(const std::wstring& url)
{
    if (url.rfind(L"https://", 0) == 0 || url.rfind(L"http://", 0) == 0)
        return GitUrlType::Https;
    if (url.find(L'@') != std::wstring::npos && url.find(L':') != std::wstring::npos)
        return GitUrlType::Ssh;
    return GitUrlType::Invalid;
}

static std::wstring BuildAuthUrl(const std::wstring& url, const std::wstring& token)
{
    if (token.empty()) return url;
    if (ClassifyGitUrl(url) != GitUrlType::Https) return url;
    size_t schemeEnd = url.find(L"://");
    if (schemeEnd == std::wstring::npos) return url;
    schemeEnd += 3;
    return url.substr(0, schemeEnd) + L"x-access-token:" + token + L"@" + url.substr(schemeEnd);
}

static Gdiplus::Image* LoadPngFromResource(HINSTANCE hInstance, int resourceId, IStream** outStream)
{
    if (outStream) *outStream = nullptr;
    if (!hInstance) return nullptr;

    HRSRC hResource = FindResourceW(hInstance, MAKEINTRESOURCEW(resourceId), L"PNG");
    if (!hResource) return nullptr;

    HGLOBAL hLoaded = LoadResource(hInstance, hResource);
    if (!hLoaded) return nullptr;

    DWORD size = SizeofResource(hInstance, hResource);
    const void* data = LockResource(hLoaded);
    if (!data || size == 0) return nullptr;

    // CreateStreamOnHGlobal precisa de um HGLOBAL alocado com GlobalAlloc
    // (nao pode ser o retornado por LoadResource, que e read-only).
    HGLOBAL hBuffer = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!hBuffer) return nullptr;

    void* buffer = GlobalLock(hBuffer);
    if (buffer) {
        memcpy(buffer, data, size);
        GlobalUnlock(hBuffer);
    }

    IStream* stream = nullptr;
    Gdiplus::Image* image = nullptr;
    if (SUCCEEDED(CreateStreamOnHGlobal(hBuffer, TRUE, &stream))) {
        // TRUE = stream libera hBuffer quando for liberado.
        // Mantemos o stream vivo (nao damos Release aqui) porque GDI+
        // pode acessar os dados depois. Ele sera liberado no shutdown.
        image = Gdiplus::Image::FromStream(stream, FALSE);
        if (image && image->GetLastStatus() == Gdiplus::Ok) {
            if (outStream) *outStream = stream;  // caller libera depois
            else stream->Release();
        } else {
            delete image;
            image = nullptr;
            stream->Release();
        }
    } else {
        GlobalFree(hBuffer);
    }

    return image;
}

constexpr UINT WM_BRANCHES_READY = WM_APP + 2;
struct BranchLookupResult {
    std::vector<std::wstring> branches;
    std::wstring url;
};

static unsigned __stdcall BranchLookupThread(void* rawArg)
{
    std::unique_ptr<std::wstring> urlPtr(static_cast<std::wstring*>(rawArg));
    const std::wstring& url = *urlPtr;

    if (!IsValidGitUrl(url)) {
        g_branchLookupRunning = false;
        return 0;
    }

    std::wstring output;
    std::wstring cmd = L"git ls-remote --heads \"" + url + L"\"";
    bool ok = RunCommand(cmd, L"", output);

    if (!ok) {
        std::wstring token = GetWindowTextStr(g_hEditToken);
        if (!token.empty()) {
            std::wstring authUrl = BuildAuthUrl(url, token);
            cmd = L"git ls-remote --heads \"" + authUrl + L"\"";
            ok = RunCommand(cmd, L"", output);
        }
    }

    auto* result = new BranchLookupResult();
    result->url = url;
    if (ok) {
        std::wistringstream wiss(output);
        std::wstring line;
        while (std::getline(wiss, line)) {
            size_t tab = line.find(L'\t');
            if (tab == std::wstring::npos) continue;
            std::wstring ref = line.substr(tab + 1);
            const wchar_t prefix[] = L"refs/heads/";
            const size_t prefixLen = sizeof(prefix) / sizeof(wchar_t) - 1;
            if (ref.compare(0, prefixLen, prefix) == 0) {
                result->branches.push_back(ref.substr(prefixLen));
            }
        }
    }

    if (url == g_branchLookupUrl) {
        PostMessageW(g_hMainWnd, WM_BRANCHES_READY, reinterpret_cast<WPARAM>(result), 0);
    } else {
        delete result;
    }
    g_branchLookupRunning = false;
    return 0;
}

static void RequestBranchLookup(const std::wstring& url)
{
    if (url.empty()) return;
    g_branchLookupUrl = url;
    if (g_branchLookupRunning) return;
    g_branchLookupRunning = true;
    auto* urlArg = new std::wstring(url);
    _beginthreadex(nullptr, 0, &BranchLookupThread, urlArg, 0, nullptr);
}

// Subclass do STATIC do logo: pinta o PNG e abre o site ao clicar.
static LRESULT CALLBACK LogoSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
    UINT_PTR /*uIdSubclass*/, DWORD_PTR /*dwRefData*/)
{
    switch (msg)
    {
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        if (g_logoImage)
        {
            Gdiplus::Graphics graphics(hdc);
            graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            const int imgW = static_cast<int>(g_logoImage->GetWidth());
            const int imgH = static_cast<int>(g_logoImage->GetHeight());
            RECT rc;
            GetClientRect(hwnd, &rc);

            // "Scale to fit": maximiza o logo dentro da caixa mantendo aspect ratio.
            // Se o PNG for menor que a caixa, aumenta; se for maior, diminui.
            int drawW = imgW;
            int drawH = imgH;
            if (imgW > 0 && imgH > 0 && rc.right > 0 && rc.bottom > 0)
            {
                const float scaleX = static_cast<float>(rc.right)  / imgW;
                const float scaleY = static_cast<float>(rc.bottom) / imgH;
                const float scale  = (scaleX < scaleY) ? scaleX : scaleY;
                drawW = static_cast<int>(imgW * scale);
                drawH = static_cast<int>(imgH * scale);
            }

            const int x = (rc.right  - drawW) / 2;   // centraliza horizontal
            const int y = (rc.bottom - drawH) / 2;    // centraliza vertical
            graphics.DrawImage(g_logoImage, x, y, drawW, drawH);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_SETCURSOR:
    {
        static HCURSOR hHand = LoadCursorW(nullptr, IDC_HAND);
        SetCursor(hHand);
        return TRUE;
    }
    case WM_LBUTTONDOWN:
    {
        ShellExecuteW(hwnd, L"open", LOGO_URL, nullptr, nullptr, SW_SHOWNORMAL);
        return 0;
    }
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

static LRESULT CALLBACK EditSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
    UINT_PTR /*uIdSubclass*/, DWORD_PTR /*dwRefData*/)
{
    if (msg == WM_GETDLGCODE)
        return DLGC_WANTALLKEYS;

    if (msg == WM_KEYDOWN && wParam == L'A' && (GetKeyState(VK_CONTROL) & 0x8000))
    {
        SendMessageW(hwnd, EM_SETSEL, 0, -1);
        return 0;
    }

    // Bloqueia Enter nos EDITs single-line (agora ES_MULTILINE para EM_SETRECT):
    // sem isso, o usuario poderia inserir quebra de linha acidental.
    if (msg == WM_KEYDOWN && wParam == VK_RETURN)
        return 0;
    if (msg == WM_CHAR && wParam == 0x0D)  // carriage return
        return 0;
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
//  Janela principal
// ---------------------------------------------------------------------------

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
    {
        RECT rcClient{};
        GetClientRect(hwnd, &rcClient);
        const int clientH = rcClient.bottom;

        // Todas as fontes usam CLEARTYPE_QUALITY para forcar anti-aliasing
        // (DEFAULT_QUALITY deixa o Windows decidir, o que as vezes resulta em
        // fontes pixeladas em configs DPI altas ou fontes pequenas).
        g_hFontRegular = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        g_hFontBold = CreateFontW(-14, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        // Titulo do app: size 24 black (FW_BLACK = 900).
        g_hFontTitle = CreateFontW(-24, 0, 0, 0, FW_BLACK, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        // Log box: size 12 bold (azul #00B6FF aplicado via WM_CTLCOLORSTATIC).
        g_hFontLog = CreateFontW(-12, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Consolas");

        // ---------------- Barra lateral "Meus repos" ----------------
        HWND hSidebarLabel = CreateWindowW(L"STATIC", L"Meus repos", WS_VISIBLE | WS_CHILD,
            SIDEBAR_X + 60, 15, SIDEBAR_WIDTH - 40, 24, hwnd, nullptr, nullptr, nullptr);

        g_hBtnAddRepo = CreateWindowW(L"BUTTON", L"+", WS_VISIBLE | WS_CHILD | BS_OWNERDRAW,
            SIDEBAR_X + SIDEBAR_WIDTH - 30, 12, 30, 30, hwnd, CtrlId(ID_BTN_ADD_REPO), nullptr, nullptr);
        // Subclass do botao "+": BS_OWNERDRAW nao recebe ODS_HOTLIGHT sozinho.
        // Trackeamos hover/pressed manualmente via g_plusButtonState.
        SetWindowSubclass(g_hBtnAddRepo, AddRepoButtonSubclassProc, 8, 0);

        const int listY = 46;
        // Reserva 70px no fundo da sidebar para o logo Persia Studio.
        const int logoBottomReserve = 70;
        const int listH = (clientH - listY - logoBottomReserve > 60)
                       ? (clientH - listY - logoBottomReserve) : 60;

        g_hListRepos = CreateWindowW(L"LISTBOX", L"",
            WS_VISIBLE | WS_CHILD | WS_BORDER | WS_VSCROLL |
            LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS,
            SIDEBAR_X, listY, SIDEBAR_WIDTH, listH,
            hwnd, CtrlId(ID_LIST_REPOS), nullptr, nullptr);

        SendMessageW(g_hListRepos, LB_SETITEMHEIGHT, 0, 30);
        SetWindowSubclass(g_hListRepos, RepoListSubclassProc, 1, 0);

        // ---------------- Logo Persia Studio (rodape da sidebar, clicavel) ----------------
        const int logoH = 68;
        const int logoY = clientH - logoH - 18;
        g_hwndLogo = CreateWindowW(L"STATIC", L"",
            WS_VISIBLE | WS_CHILD | SS_NOTIFY,
            SIDEBAR_X, logoY, SIDEBAR_WIDTH, logoH,
            hwnd, nullptr, nullptr, nullptr);
        SetWindowSubclass(g_hwndLogo, LogoSubclassProc, 7, 0);

        // Carrega o logo do recurso PNG embutido no EXE (IDB_LOGO).
        // Se o recurso nao existir (ex.: esqueceu de adicionar o .rc),
        // o STATIC fica vazio mas o clique continua funcionando (abre o site).
        g_logoImage = LoadPngFromResource(g_hInstance, IDB_LOGO, &g_logoStream);

        // Carrega o background (IDB_BG) do recurso PNG embutido no EXE.
        // Se o recurso nao existir, a janela usa o brush padrao do Windows.
        g_bgImage = LoadPngFromResource(g_hInstance, IDB_BG, &g_bgStream);

        const int sprPlusIds[3]  = { IDB_SPR_PLUS_0,  IDB_SPR_PLUS_1,  IDB_SPR_PLUS_2 };
        const int sprTrashIds[3] = { IDB_SPR_TRASH_0, IDB_SPR_TRASH_1, IDB_SPR_TRASH_2 };
        for (int i = 0; i < 3; ++i) {
            g_sprPlus[i]  = LoadPngFromResource(g_hInstance, sprPlusIds[i],  &g_sprPlusStream[i]);
            g_sprTrash[i] = LoadPngFromResource(g_hInstance, sprTrashIds[i], &g_sprTrashStream[i]);
        }

        // Cria o brush de fundo dos EDITs (cinza claro) uma unica vez.
        g_hbrEditBg = CreateSolidBrush(EDIT_BG_COLOR);

        // ---------------- Conteúdo principal (deslocado para a direita) ----------------
        int y = 44; // +4 pixels (alinhamento) — NAO mudar (layout original)

        // Titulo do app: "EasyGitPusher" em size 20 black (FW_BLACK).
        // Posicionado no espaco vazio ACIMA de "Pasta a enviar:" (que
        // continua em y=44). Nao empurra nada para baixo — aproveita o
        // mesmo top-margin onde a sidebar tem o rotulo "Meus repos" em y=15.
        // Altura 30px para acomodar uma fonte de 20pt (~26.7px) sem clipping.
        // (Valores ajustados: y=9, h=32 — posicao final do titulo.)
        HWND hTitle = CreateWindowW(L"STATIC", L"EasyGitPusher", WS_VISIBLE | WS_CHILD,
            MAIN_X, 9, 400, 32, hwnd, nullptr, nullptr, nullptr);

        CreateWindowW(L"STATIC", L"Pasta a enviar:", WS_VISIBLE | WS_CHILD,
            MAIN_X, y, 200, 20, hwnd, nullptr, nullptr, nullptr);
        y += 19;
        // WS_EX_CLIENTEDGE (em vez de WS_BORDER) desenha a borda FORA do
        // client area — resolve o bug do bg vazar 1px e o texto descentrado.
        g_hEditFolder = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_VISIBLE | WS_CHILD | ES_AUTOHSCROLL,
            MAIN_X, y, 480, 26, hwnd, CtrlId(ID_EDIT_FOLDER), nullptr, nullptr);
        HWND hBrowse = CreateWindowW(L"BUTTON", L"Procurar...",
            WS_VISIBLE | WS_CHILD, MAIN_X + 490, y, 100, 26,
            hwnd, CtrlId(ID_BTN_BROWSE), nullptr, nullptr);
        y += 34;

        CreateWindowW(L"STATIC",
            L"Link do repositório (ex: https://github.com/usuário/repo.git):",
            WS_VISIBLE | WS_CHILD, MAIN_X, y, 480, 20, hwnd, nullptr, nullptr, nullptr);
        y += 19;
        g_hEditRepo = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_VISIBLE | WS_CHILD | ES_AUTOHSCROLL,
            MAIN_X, y, 590, 26, hwnd, CtrlId(ID_EDIT_REPO), nullptr, nullptr);
        y += 34;

        CreateWindowW(L"STATIC", L"Token de acesso (Personal Access Token):",
            WS_VISIBLE | WS_CHILD, MAIN_X, y, 480, 20, hwnd, nullptr, nullptr, nullptr);
        y += 19;
        // Layout: token EDIT (266 = 480 - 214, conforme solicitado)
        // + "Mostrar" (70) + dropdown branch (210).
        constexpr int TOKEN_EDIT_W = 280;
        constexpr int TOKEN_CHK_W  = 69;
        constexpr int TOKEN_CHK_X  = MAIN_X + TOKEN_EDIT_W + 8;
        constexpr int BRANCH_COMBO_W = 226;
        constexpr int BRANCH_COMBO_X = TOKEN_CHK_X + TOKEN_CHK_W + 8;

        g_hEditToken = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_VISIBLE | WS_CHILD | ES_AUTOHSCROLL | ES_PASSWORD,
            MAIN_X, y, TOKEN_EDIT_W, 26, hwnd, CtrlId(ID_EDIT_TOKEN), nullptr, nullptr);
        g_hChkShowToken = CreateWindowW(L"BUTTON", L"Mostrar",
            WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX, TOKEN_CHK_X, y + 1, TOKEN_CHK_W, 26,
            hwnd, CtrlId(ID_CHK_SHOWTOKEN), nullptr, nullptr);

        CreateWindowW(L"STATIC", L"Selecione a Branch:",
            WS_VISIBLE | WS_CHILD, BRANCH_COMBO_X, y - 19, 200, 20, hwnd, nullptr, nullptr, nullptr);

        g_hComboBranch = CreateWindowW(L"COMBOBOX", L"",
            WS_VISIBLE | WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL,
            BRANCH_COMBO_X, y, BRANCH_COMBO_W, 200,
            hwnd, CtrlId(ID_COMBO_BRANCH), nullptr, nullptr);

        SendMessageW(g_hComboBranch, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"(branch)"));
        SendMessageW(g_hComboBranch, CB_SETCURSEL, 0, 0);
        y += 34;

        CreateWindowW(L"STATIC", L"Mensagem do commit (opcional):",
            WS_VISIBLE | WS_CHILD, MAIN_X, y, 480, 20, hwnd, nullptr, nullptr, nullptr);
        y += 20;
        g_hEditCommitMsg = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_VISIBLE | WS_CHILD | ES_AUTOHSCROLL,
            MAIN_X, y, 590, 26, hwnd, CtrlId(ID_EDIT_COMMITMSG), nullptr, nullptr);
        y += 34;

        HWND hPush = CreateWindowW(L"BUTTON", L"PUSH",
            WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON, MAIN_X, y, 590, 30,
            hwnd, CtrlId(ID_BTN_PUSH), nullptr, nullptr);
        y += 34;

        CreateWindowW(L"STATIC", L"Log do Git:", WS_VISIBLE | WS_CHILD,
            MAIN_X, y, 100, 20, hwnd, nullptr, nullptr, nullptr);
        y += 20;
        g_hEditLog = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_VISIBLE | WS_CHILD | WS_VSCROLL |
            ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
            MAIN_X, y, 590, 166,
            hwnd, CtrlId(ID_EDIT_LOG), nullptr, nullptr);

        // Texto de rodape (placeholder editavel). Edite FOOTER_TEXT nas globals
        // para mudar o conteudo; FOOTER_X e FOOTER_Y_OFFSET para mudar a posicao.
        RECT clientRect{};
        GetClientRect(hwnd, &clientRect);
        const int footerY = clientRect.bottom + FOOTER_Y_OFFSET;
        g_hwndFooter = CreateWindowW(L"STATIC", FOOTER_TEXT,
            WS_VISIBLE | WS_CHILD,
            FOOTER_X, footerY, 590, 20,
            hwnd, CtrlId(ID_STATIC_FOOTER), nullptr, nullptr);

        HWND controls[] = { g_hEditFolder, hBrowse, g_hEditRepo, g_hEditToken,
                            g_hChkShowToken, g_hEditCommitMsg, hPush, g_hEditLog,
                            hSidebarLabel, g_hBtnAddRepo, g_hComboBranch, g_hwndFooter };
        for (HWND c : controls)
            SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontRegular), TRUE);
        SendMessageW(hSidebarLabel, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontBold), TRUE);
        // Fonte do titulo do app (size 20 black).
        SendMessageW(hTitle, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontTitle), TRUE);
        // Fonte do log: size 12 bold, cor azul aplicada via WM_CTLCOLORSTATIC.
        SendMessageW(g_hEditLog, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontLog), TRUE);

        // Padding horizontal dentro dos EDITs single-line.
        // EM_SETMARGINS funciona nativamente em EDITs single-line (sem ES_MULTILINE)
        // e NAO quebra o auto-scroll-into-view do caret (diferente de EM_SETRECT
        // + ES_MULTILINE, que escondia o texto "embaixo da capa" quando o usuario
        // tentava navegar com as setas para alem da area visivel).
        // LWORD = left margin, HIWORD = right margin (em pixels).
        HWND singleLineEdits[] = { g_hEditFolder, g_hEditRepo, g_hEditToken, g_hEditCommitMsg };
        for (HWND e : singleLineEdits)
        {
            SendMessageW(e, EM_SETMARGINS,
                          EC_LEFTMARGIN | EC_RIGHTMARGIN,
                          MAKELONG(4, 4));
        }

        SendMessageW(g_hEditLog, EM_SETLIMITTEXT, 0, 0);

        // Habilita Ctrl+A (selecionar tudo) em todos os campos EDIT
        SetWindowSubclass(g_hEditFolder,    EditSubclassProc, 2, 0);
        SetWindowSubclass(g_hEditRepo,      EditSubclassProc, 3, 0);
        SetWindowSubclass(g_hEditToken,     EditSubclassProc, 4, 0);
        SetWindowSubclass(g_hEditCommitMsg, EditSubclassProc, 5, 0);
        SetWindowSubclass(g_hEditLog,       EditSubclassProc, 6, 0);

        // Carrega os perfis salvos e restaura o último selecionado
        LoadProfilesConfig();
        RebuildRepoList();
        if (g_selectedProfile >= 0)
            LoadProfileIntoFields(g_selectedProfile);

        return 0;
    }

    case WM_COMMAND:
    {
        const int id = LOWORD(wParam);
        const int code = HIWORD(wParam);

        if (id == ID_BTN_BROWSE && code == BN_CLICKED)
        {
            BrowseForFolder();
        }
        else if (id == ID_CHK_SHOWTOKEN && code == BN_CLICKED)
        {
            const bool checked = (SendMessageW(g_hChkShowToken, BM_GETCHECK, 0, 0) == BST_CHECKED);
            LONG_PTR style = GetWindowLongPtrW(g_hEditToken, GWL_STYLE);
            if (checked) style &= ~ES_PASSWORD; else style |= ES_PASSWORD;
            SetWindowLongPtrW(g_hEditToken, GWL_STYLE, style);
            SendMessageW(g_hEditToken, EM_SETPASSWORDCHAR, checked ? 0 : L'*', 0);
            InvalidateRect(g_hEditToken, nullptr, TRUE);
        }
        else if (id == ID_EDIT_REPO && code == EN_CHANGE)
        {
            SetTimer(hwnd, 1001, 500, nullptr);  // debounce 500ms
        }
        else if (id == ID_EDIT_TOKEN && code == EN_CHANGE)
        {
            if (!GetWindowTextStr(g_hEditRepo).empty())
                SetTimer(hwnd, 1001, 500, nullptr);
        }
        else if (id == ID_BTN_PUSH && code == BN_CLICKED)
        {
            DoPush();
        }
        else if (id == ID_BTN_ADD_REPO && code == BN_CLICKED)
        {
            AddCurrentAsProfile();
        }
        else if (id == ID_LIST_REPOS && code == LBN_SELCHANGE)
        {
            const int index = (int)SendMessageW(g_hListRepos, LB_GETCURSEL, 0, 0);
            if (index != LB_ERR)
            {
                g_selectedProfile = index;
                LoadProfileIntoFields(index);
                SaveProfilesConfig(); // persiste qual repositório foi selecionado por último
            }
        }
        return 0;
    }

    case WM_DRAWITEM:
    {
        LPDRAWITEMSTRUCT dis = reinterpret_cast<LPDRAWITEMSTRUCT>(lParam);
        if (dis->CtlID == ID_BTN_ADD_REPO)
        {
            // Estado vem do subclass que rastreia hover/pressed manualmente
            // (BS_OWNERDRAW nao envia ODS_HOTLIGHT sozinho).
            int state = g_plusButtonState;
            if (state < 0 || state > 2) state = 0;
            if (g_sprPlus[state]) {
                Gdiplus::Graphics graphics(dis->hDC);
                graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
                const int imgW = static_cast<int>(g_sprPlus[state]->GetWidth());
                const int imgH = static_cast<int>(g_sprPlus[state]->GetHeight());
                const int rcW = dis->rcItem.right  - dis->rcItem.left;
                const int rcH = dis->rcItem.bottom - dis->rcItem.top;
                int drawW = imgW, drawH = imgH;
                if (imgW > 0 && imgH > 0 && rcW > 0 && rcH > 0) {
                    const float sx = static_cast<float>(rcW) / imgW;
                    const float sy = static_cast<float>(rcH) / imgH;
                    const float s = (sx < sy) ? sx : sy;
                    drawW = static_cast<int>(imgW * s);
                    drawH = static_cast<int>(imgH * s);
                }
                const int x = dis->rcItem.left + (rcW - drawW) / 2;
                const int y = dis->rcItem.top  + (rcH - drawH) / 2;
                graphics.DrawImage(g_sprPlus[state], x, y, drawW, drawH);
            } else {
                DefWindowProcW(hwnd, msg, wParam, lParam);
            }
            return TRUE;
        }
        if (dis->CtlID == ID_LIST_REPOS)
        {
            OnDrawRepoItem(dis);
            return TRUE;
        }
        return FALSE;
    }

    case WM_PUSH_DONE:
    {
        g_isPushing = false;
        EnableWindow(GetDlgItem(g_hMainWnd, ID_BTN_PUSH), TRUE);
        SetWindowTextW(GetDlgItem(g_hMainWnd, ID_BTN_PUSH), L"PUSH");

        if (g_lastPushSuccess)
            MessageBoxW(g_hMainWnd, L"Push realizado com sucesso!",
                L"Concluído", MB_OK | MB_ICONINFORMATION);
        else
            MessageBoxW(g_hMainWnd, L"O push falhou. Veja o log na janela para detalhes.",
                L"Erro", MB_OK | MB_ICONERROR);
        return 0;
    }

    case WM_CTLCOLORSTATIC:
    {
        const HDC hdc = reinterpret_cast<HDC>(wParam);
        const HWND hwndCtl = reinterpret_cast<HWND>(lParam);
        // EDITs com ES_READONLY + ES_MULTILINE as vezes caem aqui em vez
        // de WM_CTLCOLOREDIT. Se for o nosso log box, devolve brush SOLIDO
        // (#212121) em vez de HOLLOW_BRUSH para evitar o glitch transparente.
        if (hwndCtl == g_hEditLog) {
            // Log: texto azul #00B6FF (mesmo azul dos labels) em negrito.
            SetTextColor(hdc, LABEL_TEXT_COLOR);
            SetBkColor(hdc, EDIT_BG_COLOR);
            return reinterpret_cast<LRESULT>(g_hbrEditBg);
        }
        SetBkMode(hdc, TRANSPARENT);
        if (GetDlgCtrlID(hwndCtl) == ID_STATIC_FOOTER)
            SetTextColor(hdc, FOOTER_TEXT_COLOR);
        else
            SetTextColor(hdc, LABEL_TEXT_COLOR);
        return reinterpret_cast<LRESULT>(GetStockObject(HOLLOW_BRUSH));
    }
    case WM_CTLCOLOREDIT:
    {
        // Caixas de texto (EDIT): texto branco sobre fundo #212121.
        const HDC hdc = reinterpret_cast<HDC>(wParam);
        SetTextColor(hdc, RGB(240, 240, 240));
        SetBkColor(hdc, EDIT_BG_COLOR);
        return reinterpret_cast<LRESULT>(g_hbrEditBg);
    }
    case WM_CTLCOLORLISTBOX:
    {
        // Listbox da sidebar: fundo #212121, texto branco.
        const HDC hdc = reinterpret_cast<HDC>(wParam);
        SetTextColor(hdc, RGB(240, 240, 240));
        SetBkColor(hdc, EDIT_BG_COLOR);
        return reinterpret_cast<LRESULT>(g_hbrEditBg);
    }

    case WM_TIMER:
    {
        KillTimer(hwnd, wParam);
        if (wParam == 1001) {
            const std::wstring url = GetWindowTextStr(g_hEditRepo);
            RequestBranchLookup(url);
        }
        return 0;
    }
    case WM_BRANCHES_READY:
    {
        std::unique_ptr<BranchLookupResult> result(reinterpret_cast<BranchLookupResult*>(wParam));
        const std::wstring currentUrl = GetWindowTextStr(g_hEditRepo);
        if (result->url != currentUrl) return 0;

        SendMessageW(g_hComboBranch, CB_RESETCONTENT, 0, 0);
        g_remoteBranches.clear();
        if (result->branches.empty()) {
            SendMessageW(g_hComboBranch, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"(branch)"));
            SendMessageW(g_hComboBranch, CB_SETCURSEL, 0, 0);
        } else {
            int selectIdx = 0;
            for (size_t i = 0; i < result->branches.size(); ++i) {
                SendMessageW(g_hComboBranch, CB_ADDSTRING, 0,
                    reinterpret_cast<LPARAM>(result->branches[i].c_str()));
                g_remoteBranches.push_back(result->branches[i]);
                if (result->branches[i] == L"main")  selectIdx = static_cast<int>(i);
            }
            SendMessageW(g_hComboBranch, CB_SETCURSEL, selectIdx, 0);
        }
        return 0;
    }
    case WM_ERASEBKGND:
    {
        // Desenha o bg.png esticado para preencher toda a area cliente.
        // Se a imagem nao foi carregada, deixa o Windows pintar padrao.
        const HDC hdc = reinterpret_cast<HDC>(wParam);
        if (g_bgImage)
        {
            RECT rc;
            GetClientRect(hwnd, &rc);
            Gdiplus::Graphics graphics(hdc);
            graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            // DrawImage(image, dstX, dstY, dstW, dstH) estica para o retangulo.
            graphics.DrawImage(g_bgImage, 0, 0, rc.right, rc.bottom);
            return 1;  // ja tratamos, nao deixar o Windows pintar por cima
        }
        return 0;  // deixa o Windows pintar com o brush padrao
    }
    case WM_DESTROY:
        if (g_hFontRegular) DeleteObject(g_hFontRegular);
        if (g_hFontBold) DeleteObject(g_hFontBold);
        if (g_hFontTitle) DeleteObject(g_hFontTitle);
        if (g_hFontLog) DeleteObject(g_hFontLog);
        if (g_hbrEditBg) { DeleteObject(g_hbrEditBg); g_hbrEditBg = nullptr; }
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
//  Entrada do programa
// ---------------------------------------------------------------------------

int APIENTRY wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int nCmdShow)
{
    // Inicializa GDI+ (usado para carregar/desenhar o PNG do logo).
    ULONG_PTR gdiplusToken = 0;
    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, nullptr);

    g_hInstance = hInstance;  // salvo para LoadPngFromResource usar no WM_CREATE

    const HRESULT hrCo = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hrCo) && hrCo != RPC_E_CHANGED_MODE)
    {
        MessageBoxW(nullptr, L"Falha ao inicializar COM.", L"Erro", MB_OK | MB_ICONERROR);
        return 1;
    }

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    static const wchar_t className[] = L"EasyGitPusherWindowClass";

    // Usamos WNDCLASSEXW para poder definir hIconSm (icone 16x16 na barra de
    // titulo / alt+tab pequeno). WNDCLASSW (sem EX) não tem esse campo.
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = className;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    // Icone da janela: IDI_GITPUSHER (favicon.ico embutido no .rc).
    // Aparece na barra de titulo, alt+tab e barra de tarefas.
    wc.hIcon   = static_cast<HICON>(LoadImageW(hInstance,
                                      MAKEINTRESOURCEW(IDI_GITPUSHER),
                                      IMAGE_ICON, 0, 0,
                                      LR_DEFAULTSIZE | LR_SHARED));
    wc.hIconSm = static_cast<HICON>(LoadImageW(hInstance,
                                      MAKEINTRESOURCEW(IDI_GITPUSHER),
                                      IMAGE_ICON, 16, 16,
                                      LR_DEFAULTCOLOR | LR_SHARED));

    RegisterClassExW(&wc);

    //JANELA FICA AQUI - centralizada na area de trabalho (exclui taskbar)
    RECT rcWork{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &rcWork, 0);
    const int winX = rcWork.left + ((rcWork.right - rcWork.left) - 900) / 2;
    const int winY = rcWork.top  + ((rcWork.bottom - rcWork.top) - 550) / 2;
    g_hMainWnd = CreateWindowExW(
        0, className, L"EasyGitPusher",
        WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX & ~WS_THICKFRAME,
        winX, winY, 900, 550,
        nullptr, nullptr, hInstance, nullptr
    );

    ShowWindow(g_hMainWnd, nCmdShow);
    UpdateWindow(g_hMainWnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // Libera as imagens (logo + bg) e os streams ANTES de desligar o GDI+.
    if (g_logoImage) { delete g_logoImage; g_logoImage = nullptr; }
    if (g_logoStream) { g_logoStream->Release(); g_logoStream = nullptr; }
    if (g_bgImage)   { delete g_bgImage;   g_bgImage   = nullptr; }
    if (g_bgStream)   { g_bgStream->Release();   g_bgStream   = nullptr; }
    for (int i = 0; i < 3; ++i) {
        if (g_sprPlus[i])  { delete g_sprPlus[i];  g_sprPlus[i]  = nullptr; }
        if (g_sprPlusStream[i])  { g_sprPlusStream[i]->Release();  g_sprPlusStream[i]  = nullptr; }
        if (g_sprTrash[i]) { delete g_sprTrash[i]; g_sprTrash[i] = nullptr; }
        if (g_sprTrashStream[i]) { g_sprTrashStream[i]->Release(); g_sprTrashStream[i] = nullptr; }
    }
    Gdiplus::GdiplusShutdown(gdiplusToken);

    CoUninitialize();
    return 0;
}