# AI Flight Diagnostics forum publishing checklist

The forum update is intentionally prepared but not posted automatically. Publish it in this order so the original post can link to the final reply URL.

1. Open the existing topic and confirm that the current final reply is still post 4.
2. Copy `AI_ASSISTANT_FORUM_UPDATE_EN.md` into a new reply.
3. At each image marker, upload the corresponding local asset through the Discourse composer and let Discourse replace the repository-relative Markdown link:
   - `docs/assets/ai-assistant/ai-flight-diagnostics-analyze-en.png`
   - `docs/assets/ai-assistant/ai-flight-diagnostics-data-flow.svg`
   - `docs/assets/ai-assistant/ai-assistant-settings.png`
   - `docs/assets/ai-assistant/ai-flight-diagnostics-provider-safety.svg`
4. Preview the reply and verify the table, numbered lists, four images, source branch, three commit links, and historical Release link.
5. Publish the reply. If it is not post 5, replace `/49228/5` in both main-post notice files and both historical article drafts with the actual reply URL.
6. Edit the first post and insert `AI_ASSISTANT_FORUM_MAIN_POST_NOTICE_EN.md` immediately below the title and before the original subtitle.
7. Preview the first post, confirm that its update link opens the new reply, and save the edit.

Before publishing, confirm that the screenshots still contain only the example endpoint and masked API key. The bilingual VitePress documentation build was rerun successfully while preparing this update. Do not describe the focused tests, Debug application build, or SITL matrix as newly passing unless they have been rerun in a compatible environment.
