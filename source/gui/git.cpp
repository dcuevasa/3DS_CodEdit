#include <cstring>

#include "c2d_helper.h"
#include "colours.h"
#include "config.h"
#include "git/git.h"
#include "gui.h"
#include "osk.h"
#include "touch.h"
#include "utils.h"

namespace GUI {
    static int git_selected = 0;
    static u64 git_repeat_timestamp = 0;
    static bool git_needs_refresh = true;

    static bool git_repo_found = false;
    static std::string git_repo_root = "-";
    static std::string git_branch = "-";
    static int git_staged_count = 0;
    static std::string git_message = "A to run action, B to return";

    static constexpr int git_action_count = 5;
    static const char *git_actions[git_action_count] = {
        "Init repository here",
        "Refresh repository status",
        "Add all",
        "Commit staged",
        "Sync remote (WIP)"
    };

    static void RefreshGitStatus(void) {
        char repo_root[1024] = { 0 };
        char branch[128] = { 0 };

        git_repo_found = git_find_repository_root(cfg.cwd.c_str(), repo_root, sizeof(repo_root));
        if (!git_repo_found) {
            git_repo_root = "-";
            git_branch = "-";
            git_staged_count = 0;
            git_message = "No repository in current path";
            return;
        }

        git_repo_root = repo_root;

        if (git_get_current_branch(repo_root, branch, sizeof(branch)) == GIT_RESULT_OK)
            git_branch = branch;
        else
            git_branch = "unknown";

        if (git_get_staged_count(repo_root, &git_staged_count) != GIT_RESULT_OK)
            git_staged_count = 0;

        git_message = "Repository detected";
    }

    static void RunGitAction(MenuItem *item) {
        char message[160] = { 0 };

        switch (git_selected) {
            case 0: {
                GitResult result = git_init_repository(cfg.cwd.c_str(), message, sizeof(message));
                if (result == GIT_RESULT_OK)
                    git_message = message;
                else
                    git_message = std::string("Init failed: ") + message;

                git_needs_refresh = true;
                break;
            }

            case 1:
                git_needs_refresh = true;
                RefreshGitStatus();
                break;

            case 2:
                if (git_add_all(cfg.cwd.c_str(), message, sizeof(message)) == GIT_RESULT_OK)
                    git_message = message;
                else
                    git_message = std::string("Add failed: ") + message;

                git_needs_refresh = true;
                break;

            case 3: {
                std::string commit_message = OSK::GetText("", "Commit message");
                char commit_oid[GIT_OID_HEX_LEN + 1] = { 0 };

                if (commit_message.empty()) {
                    git_message = "Commit cancelled";
                    break;
                }

                if (git_commit(cfg.cwd.c_str(), commit_message.c_str(), commit_oid, sizeof(commit_oid), message, sizeof(message)) == GIT_RESULT_OK) {
                    std::string short_oid = commit_oid;
                    if (short_oid.length() > 7)
                        short_oid.resize(7);

                    git_message = std::string("Commit ") + short_oid + " created";
                }
                else {
                    git_message = std::string("Commit failed: ") + message;
                }

                git_needs_refresh = true;
                break;
            }

            case 4:
                git_message = "Remote sync implementation pending";
                break;

            default:
                break;
        }

        (void)item;
    }

    void DisplayGitView(MenuItem *item) {
        (void)item;

        if (git_needs_refresh) {
            RefreshGitStatus();
            git_needs_refresh = false;
        }

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

        for (int i = 0; i < git_action_count; i++) {
            const float y = 92.f + (22.f * static_cast<float>(i));
            const bool selected = i == git_selected;

            C2D::Rect(8.f, y, 304.f, 20.f,
                selected ? (cfg.dark_theme ? TITLE_COLOUR_DARK : TITLE_COLOUR)
                    : (cfg.dark_theme ? SELECTOR_COLOUR_DARK : SELECTOR_COLOUR_LIGHT));

            C2D::Textf(14.f, y + 4.f, 0.31f,
                selected ? WHITE : (cfg.dark_theme ? WHITE : TEXT_MIN_COLOUR_LIGHT), "%s", git_actions[i]);
        }

        C2D::Textf(8, 206, 0.30f, cfg.dark_theme ? TEXT_MIN_COLOUR_DARK : TEXT_MIN_COLOUR_LIGHT,
            "%.46s", git_message.c_str());
        C2D::Text(8, 220, 0.29f, cfg.dark_theme ? TEXT_MIN_COLOUR_DARK : TEXT_MIN_COLOUR_LIGHT,
            "DPad: select  A: run  B: back  X: refresh");
    }

    void ControlGitView(MenuItem *item, u32 *kDown, u32 *kHeld) {
        if ((*kDown & KEY_UP) || ((*kHeld & KEY_UP) && osGetTime() >= git_repeat_timestamp)) {
            git_selected--;
            Utils::SetBounds(&git_selected, 0, git_action_count - 1);
            git_repeat_timestamp = osGetTime() + ((*kDown & KEY_UP) ? 180 : 80);
        }
        else if ((*kDown & KEY_DOWN) || ((*kHeld & KEY_DOWN) && osGetTime() >= git_repeat_timestamp)) {
            git_selected++;
            Utils::SetBounds(&git_selected, 0, git_action_count - 1);
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
            RefreshGitStatus();
        }

        if (*kDown & KEY_TOUCH) {
            for (int i = 0; i < git_action_count; i++) {
                const u16 y1 = static_cast<u16>(92 + (22 * i));
                const u16 y2 = static_cast<u16>(y1 + 20);
                if (Touch::Rect(8, y1, 312, y2)) {
                    git_selected = i;
                    RunGitAction(item);
                    break;
                }
            }
        }
    }
}
