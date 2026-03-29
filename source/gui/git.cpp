#include <cstring>

#include "c2d_helper.h"
#include "colours.h"
#include "config.h"
#include "fs.h"
#include "git/git.h"
#include "gui.h"
#include "net.h"
#include "osk.h"
#include "touch.h"
#include "utils.h"

namespace GUI {
    static int git_selected = 0;
    static int git_action_start = 0;
    static u64 git_repeat_timestamp = 0;
    static bool git_needs_refresh = true;
    static u64 git_message_expire_at = 0;

    static bool git_repo_found = false;
    static std::string git_repo_root = "-";
    static std::string git_branch = "-";
    static int git_staged_count = 0;
    static std::string git_message = "A to run action, B to return";
    static std::string git_last_commit_message;

    static constexpr int git_action_count = 10;
    static constexpr int git_visible_actions = 5;
    static constexpr float git_actions_y = 96.f;
    static constexpr float git_action_h = 20.f;
    static constexpr float git_action_gap = 22.f;

    static const char *git_actions[git_action_count] = {
        "Init repository here",
        "Refresh repository status",
        "Add all",
        "Commit staged",
        "Probe GitHub remote",
        "Clone GitHub to current path",
        "Fetch (fast-forward check)",
        "Pull fast-forward",
        "Push to GitHub",
        "Set GitHub token"
    };

    static void EnsureGitSelectionVisible(void) {
        int max_start = git_action_count - git_visible_actions;

        if (max_start < 0)
            max_start = 0;

        if (git_selected < git_action_start)
            git_action_start = git_selected;

        if (git_selected >= (git_action_start + git_visible_actions))
            git_action_start = git_selected - git_visible_actions + 1;

        Utils::SetBounds(&git_action_start, 0, max_start);
    }

    static void SetGitMessage(const std::string &message, u32 ttl_ms = 0) {
        git_message = message;
        git_message_expire_at = (ttl_ms == 0) ? 0 : (osGetTime() + ttl_ms);
    }

    static bool BeginNetworkOperation(void) {
        if (!Net::GetNetworkStatus()) {
            SetGitMessage("Operation failed: no network connection", 5000);
            return false;
        }

        if (R_FAILED(Net::Init())) {
            SetGitMessage("Operation failed: cannot initialize network", 5000);
            return false;
        }

        return true;
    }

    static const char *TokenOrNull(void) {
        return cfg.git_pat.empty() ? nullptr : cfg.git_pat.c_str();
    }

    static void RefreshCurrentDirectory(MenuItem *item) {
        if (item == nullptr)
            return;

        if (R_SUCCEEDED(FS::GetDirList(cfg.cwd, item->entries))) {
            if (item->entries.empty())
                item->selected = 0;
            else
                Utils::SetBounds(&item->selected, 0, static_cast<int>(item->entries.size() - 1));

            ResetCheckbox(item);
        }
    }

    static void RefreshGitStatus(bool keep_message = false) {
        char repo_root[1024] = { 0 };
        char branch[128] = { 0 };

        git_repo_found = git_find_repository_root(cfg.cwd.c_str(), repo_root, sizeof(repo_root));
        if (!git_repo_found) {
            git_repo_root = "-";
            git_branch = "-";
            git_staged_count = 0;
            if (!keep_message)
                SetGitMessage("No repository in current path");
            return;
        }

        git_repo_root = repo_root;

        if (git_get_current_branch(repo_root, branch, sizeof(branch)) == GIT_RESULT_OK)
            git_branch = branch;
        else
            git_branch = "unknown";

        if (git_get_staged_count(repo_root, &git_staged_count) != GIT_RESULT_OK)
            git_staged_count = 0;

        if (!keep_message)
            SetGitMessage("Repository detected");
    }

    static void RunGitAction(MenuItem *item) {
        char message[192] = { 0 };

        switch (git_selected) {
            case 0: {
                GitResult result = git_init_repository(cfg.cwd.c_str(), message, sizeof(message));
                if (result == GIT_RESULT_OK)
                    SetGitMessage(message, 3000);
                else
                    SetGitMessage(std::string("Init failed: ") + message, 4000);

                git_needs_refresh = true;
                break;
            }

            case 1:
                git_needs_refresh = true;
                RefreshGitStatus(false);
                break;

            case 2:
                if (git_add_all(cfg.cwd.c_str(), message, sizeof(message)) == GIT_RESULT_OK)
                    SetGitMessage(message, 3000);
                else
                    SetGitMessage(std::string("Add failed: ") + message, 4000);

                git_needs_refresh = true;
                break;

            case 3: {
                std::string commit_message = OSK::GetText("", "Commit message");
                char commit_oid[GIT_OID_HEX_LEN + 1] = { 0 };

                if (commit_message.empty()) {
                    SetGitMessage("Commit cancelled", 2500);
                    break;
                }

                if (git_commit(cfg.cwd.c_str(), commit_message.c_str(), commit_oid, sizeof(commit_oid), message, sizeof(message)) == GIT_RESULT_OK) {
                    std::string short_oid = commit_oid;
                    if (short_oid.length() > 7)
                        short_oid.resize(7);

                    git_last_commit_message = commit_message;

                    SetGitMessage(std::string("Commit ") + short_oid + " created", 3500);
                }
                else {
                    SetGitMessage(std::string("Commit failed: ") + message, 4500);
                }

                git_needs_refresh = true;
                break;
            }

            case 4: {
                std::string remote_url = OSK::GetText(cfg.git_remote_url, "GitHub URL (https://github.com/owner/repo)");
                char remote_branch[128] = { 0 };
                char remote_head[GIT_OID_HEX_LEN + 1] = { 0 };

                if (remote_url.empty()) {
                    SetGitMessage("Remote probe cancelled", 2500);
                    break;
                }

                if (!BeginNetworkOperation())
                    break;

                GitResult remote_result = git_remote_probe_github(remote_url.c_str(), TokenOrNull(),
                    remote_branch, sizeof(remote_branch),
                    remote_head, sizeof(remote_head),
                    message, sizeof(message));

                Net::Exit();

                if (remote_result == GIT_RESULT_OK) {
                    std::string short_sha = remote_head;
                    if (short_sha.length() > 7)
                        short_sha.resize(7);

                    cfg.git_remote_url = remote_url;
                    cfg.git_default_branch = remote_branch;
                    Config::Save(cfg);

                    SetGitMessage(std::string("Remote OK: ") + remote_branch + " @ " + short_sha, 4500);
                }
                else {
                    SetGitMessage(std::string("Remote probe failed: ") + message, 5000);
                }

                break;
            }

            case 5: {
                std::string remote_url = OSK::GetText(cfg.git_remote_url, "GitHub URL to clone");
                std::string branch_hint = OSK::GetText(cfg.git_default_branch, "Branch (optional, empty=default)");
                char remote_branch[128] = { 0 };
                char remote_head[GIT_OID_HEX_LEN + 1] = { 0 };

                if (remote_url.empty()) {
                    SetGitMessage("Clone cancelled", 2500);
                    break;
                }

                if (!BeginNetworkOperation())
                    break;

                GitResult remote_result = git_remote_clone_github(cfg.cwd.c_str(), remote_url.c_str(),
                    branch_hint.empty() ? nullptr : branch_hint.c_str(),
                    TokenOrNull(),
                    remote_branch, sizeof(remote_branch),
                    remote_head, sizeof(remote_head),
                    message, sizeof(message));

                Net::Exit();

                if (remote_result == GIT_RESULT_OK) {
                    cfg.git_remote_url = remote_url;
                    cfg.git_default_branch = remote_branch;
                    Config::Save(cfg);

                    SetGitMessage(message, 5000);
                    git_needs_refresh = true;
                    RefreshCurrentDirectory(item);
                }
                else {
                    SetGitMessage(std::string("Clone failed: ") + message, 5500);
                }

                break;
            }

            case 6: {
                bool has_updates = false;
                std::string remote_url = cfg.git_remote_url.empty() ? OSK::GetText("", "GitHub URL for fetch") : cfg.git_remote_url;
                char remote_branch[128] = { 0 };
                char remote_head[GIT_OID_HEX_LEN + 1] = { 0 };

                if (remote_url.empty()) {
                    SetGitMessage("Fetch cancelled", 2500);
                    break;
                }

                if (!BeginNetworkOperation())
                    break;

                GitResult remote_result = git_remote_fetch_github(cfg.cwd.c_str(),
                    remote_url.c_str(),
                    cfg.git_default_branch.empty() ? nullptr : cfg.git_default_branch.c_str(),
                    TokenOrNull(),
                    &has_updates,
                    remote_branch, sizeof(remote_branch),
                    remote_head, sizeof(remote_head),
                    message, sizeof(message));

                Net::Exit();

                if (remote_result == GIT_RESULT_OK) {
                    cfg.git_remote_url = remote_url;
                    cfg.git_default_branch = remote_branch;
                    Config::Save(cfg);

                    SetGitMessage(message, 5000);
                }
                else {
                    SetGitMessage(std::string("Fetch failed: ") + message, 5500);
                }

                break;
            }

            case 7: {
                std::string remote_url = cfg.git_remote_url.empty() ? OSK::GetText("", "GitHub URL for pull") : cfg.git_remote_url;
                char remote_branch[128] = { 0 };
                char remote_head[GIT_OID_HEX_LEN + 1] = { 0 };

                if (remote_url.empty()) {
                    SetGitMessage("Pull cancelled", 2500);
                    break;
                }

                if (!BeginNetworkOperation())
                    break;

                GitResult remote_result = git_remote_pull_github(cfg.cwd.c_str(),
                    remote_url.c_str(),
                    cfg.git_default_branch.empty() ? nullptr : cfg.git_default_branch.c_str(),
                    TokenOrNull(),
                    remote_branch, sizeof(remote_branch),
                    remote_head, sizeof(remote_head),
                    message, sizeof(message));

                Net::Exit();

                if (remote_result == GIT_RESULT_OK) {
                    cfg.git_remote_url = remote_url;
                    cfg.git_default_branch = remote_branch;
                    Config::Save(cfg);

                    SetGitMessage(message, 5000);
                    git_needs_refresh = true;
                    RefreshCurrentDirectory(item);
                }
                else {
                    SetGitMessage(std::string("Pull failed: ") + message, 5500);
                }

                break;
            }

            case 8: {
                std::string remote_url = cfg.git_remote_url.empty() ? OSK::GetText("", "GitHub URL for push") : cfg.git_remote_url;
                char remote_branch[128] = { 0 };
                char remote_head[GIT_OID_HEX_LEN + 1] = { 0 };

                if (cfg.git_pat.empty()) {
                    SetGitMessage("Push failed: set GitHub token first", 4500);
                    break;
                }

                if (remote_url.empty()) {
                    SetGitMessage("Push cancelled", 2500);
                    break;
                }

                std::string push_message = git_last_commit_message;
                if (push_message.empty()) {
                    push_message = OSK::GetText("Update from 3DS_CodEdit", "Push commit message");
                    if (push_message.empty()) {
                        SetGitMessage("Push cancelled", 2500);
                        break;
                    }
                }

                if (!BeginNetworkOperation())
                    break;

                GitResult remote_result = git_remote_push_github(cfg.cwd.c_str(),
                    remote_url.c_str(),
                    cfg.git_default_branch.empty() ? nullptr : cfg.git_default_branch.c_str(),
                    cfg.git_pat.c_str(),
                    push_message.c_str(),
                    remote_branch, sizeof(remote_branch),
                    remote_head, sizeof(remote_head),
                    message, sizeof(message));

                Net::Exit();

                if (remote_result == GIT_RESULT_OK) {
                    cfg.git_remote_url = remote_url;
                    cfg.git_default_branch = remote_branch;
                    Config::Save(cfg);
                    git_last_commit_message.clear();

                    SetGitMessage(message, 5500);
                }
                else {
                    std::string push_error = message;
                    if ((push_error.find("Contents:write") != std::string::npos) ||
                        (push_error.find("HTTP 401") != std::string::npos) ||
                        (push_error.find("HTTP 403") != std::string::npos)) {
                        SetGitMessage("Push failed: token needs Contents:write on this repo", 7000);
                    }
                    else {
                        SetGitMessage(std::string("Push failed: ") + push_error, 6500);
                    }
                }

                break;
            }

            case 9: {
                std::string token = OSK::GetText(cfg.git_pat, "GitHub token (empty clears)");
                cfg.git_pat = token;
                Config::Save(cfg);

                if (cfg.git_pat.empty())
                    SetGitMessage("GitHub token cleared", 3000);
                else
                    SetGitMessage("GitHub token saved", 3000);

                break;
            }

            default:
                break;
        }

        (void)item;
    }

    void DisplayGitView(MenuItem *item) {
        (void)item;

        if (git_message_expire_at > 0 && osGetTime() >= git_message_expire_at) {
            git_message_expire_at = 0;
            RefreshGitStatus(false);
        }

        if (git_needs_refresh) {
            RefreshGitStatus(git_message_expire_at > 0);
            git_needs_refresh = false;
        }

        EnsureGitSelectionVisible();

        C2D::Rect(0, 20, 320, 220, cfg.dark_theme ? BLACK_BG : WHITE);

        C2D::Text(8, 27, 0.42f, cfg.dark_theme ? WHITE : BLACK, "Git Project");
        C2D::Textf(8, 45, 0.31f, cfg.dark_theme ? TEXT_MIN_COLOUR_DARK : TEXT_MIN_COLOUR_LIGHT,
            "Path: %.38s", cfg.cwd.c_str());

        C2D::Textf(8, 60, 0.31f, cfg.dark_theme ? TEXT_MIN_COLOUR_DARK : TEXT_MIN_COLOUR_LIGHT,
            "Repo: %s", git_repo_found ? "yes" : "no");
        C2D::Textf(82, 60, 0.31f, cfg.dark_theme ? TEXT_MIN_COLOUR_DARK : TEXT_MIN_COLOUR_LIGHT,
            "Branch: %s", git_branch.c_str());
        C2D::Textf(232, 60, 0.31f, cfg.dark_theme ? TEXT_MIN_COLOUR_DARK : TEXT_MIN_COLOUR_LIGHT,
            "Staged:%d", git_staged_count);

        C2D::Textf(8, 74, 0.31f, cfg.dark_theme ? TEXT_MIN_COLOUR_DARK : TEXT_MIN_COLOUR_LIGHT,
            "Root: %.37s", git_repo_root.c_str());

        C2D::Textf(8, 86, 0.29f, cfg.dark_theme ? TEXT_MIN_COLOUR_DARK : TEXT_MIN_COLOUR_LIGHT,
            "Remote: %.22s", cfg.git_remote_url.empty() ? "-" : cfg.git_remote_url.c_str());
        C2D::Textf(250, 86, 0.29f, cfg.dark_theme ? TEXT_MIN_COLOUR_DARK : TEXT_MIN_COLOUR_LIGHT,
            "%d/%d", git_selected + 1, git_action_count);

        for (int i = 0; i < git_visible_actions; i++) {
            const int action_index = git_action_start + i;
            const float y = git_actions_y + (git_action_gap * static_cast<float>(i));

            if (action_index >= git_action_count)
                break;

            const bool selected = action_index == git_selected;

            C2D::Rect(8.f, y, 304.f, git_action_h,
                selected ? (cfg.dark_theme ? TITLE_COLOUR_DARK : TITLE_COLOUR)
                    : (cfg.dark_theme ? SELECTOR_COLOUR_DARK : SELECTOR_COLOUR_LIGHT));

            C2D::Textf(14.f, y + 4.f, 0.31f,
                selected ? WHITE : (cfg.dark_theme ? WHITE : TEXT_MIN_COLOUR_LIGHT),
                "%s", git_actions[action_index]);
        }

        {
            std::string line1 = git_message.substr(0, 46);
            std::string line2;
            if (git_message.length() > 46)
                line2 = git_message.substr(46, 46);

            C2D::Textf(8, 200, 0.29f, cfg.dark_theme ? TEXT_MIN_COLOUR_DARK : TEXT_MIN_COLOUR_LIGHT,
                "%s", line1.c_str());

            if (!line2.empty()) {
                C2D::Textf(8, 211, 0.29f, cfg.dark_theme ? TEXT_MIN_COLOUR_DARK : TEXT_MIN_COLOUR_LIGHT,
                    "%s", line2.c_str());
            }
        }

        C2D::Text(8, 224, 0.27f, cfg.dark_theme ? TEXT_MIN_COLOUR_DARK : TEXT_MIN_COLOUR_LIGHT,
            "DPad: select  A: run  B: back  X: refresh");
    }

    void ControlGitView(MenuItem *item, u32 *kDown, u32 *kHeld) {
        if ((*kDown & KEY_UP) || ((*kHeld & KEY_UP) && osGetTime() >= git_repeat_timestamp)) {
            git_selected--;
            Utils::SetBounds(&git_selected, 0, git_action_count - 1);
            EnsureGitSelectionVisible();
            git_repeat_timestamp = osGetTime() + ((*kDown & KEY_UP) ? 180 : 80);
        }
        else if ((*kDown & KEY_DOWN) || ((*kHeld & KEY_DOWN) && osGetTime() >= git_repeat_timestamp)) {
            git_selected++;
            Utils::SetBounds(&git_selected, 0, git_action_count - 1);
            EnsureGitSelectionVisible();
            git_repeat_timestamp = osGetTime() + ((*kDown & KEY_DOWN) ? 180 : 80);
        }

        if (*kDown & KEY_A)
            RunGitAction(item);
        else if (*kDown & KEY_B) {
            item->state = MENU_STATE_TEXTREADER;
            git_needs_refresh = true;
        }
        else if (*kDown & KEY_X) {
            git_needs_refresh = true;
            RefreshGitStatus(false);
        }

        if (*kDown & KEY_TOUCH) {
            for (int i = 0; i < git_visible_actions; i++) {
                const int action_index = git_action_start + i;
                const u16 y1 = static_cast<u16>(git_actions_y + (git_action_gap * static_cast<float>(i)));
                const u16 y2 = static_cast<u16>(y1 + git_action_h);

                if (action_index >= git_action_count)
                    break;

                if (Touch::Rect(8, y1, 312, y2)) {
                    git_selected = action_index;
                    EnsureGitSelectionVisible();
                    RunGitAction(item);
                    break;
                }
            }
        }
    }
}