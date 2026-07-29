<template>
  <n-space vertical size="large" class="steam-tab w-full">
    <n-alert v-if="platform && platform !== 'linux'" type="info" :show-icon="true">
      {{ $t('steam.only_linux') }}
    </n-alert>

    <section v-if="platform === 'linux'" class="steam-section">
      <n-card class="steam-card" :segmented="{ content: true }">
        <template #header>
          <div class="steam-card-heading">
            <n-icon size="18"><i class="fab fa-steam" /></n-icon>
            <n-text strong>{{ $t('steam.status_title') }}</n-text>
          </div>
        </template>
        <n-space vertical size="large">
          <div class="steam-status-line">
            <n-text strong>{{ $t('steam.status_overall') }}</n-text>
            <n-tag size="small" :type="config.steam_sync_enable ? 'success' : 'default'">
              {{
                config.steam_sync_enable ? $t('steam.status_enabled') : $t('steam.status_disabled')
              }}
            </n-tag>
          </div>
          <n-text depth="3" class="steam-help">
            {{ gamesFoundText }}
          </n-text>
          <div class="steam-actions">
            <n-button
              size="small"
              type="primary"
              strong
              :disabled="!config.steam_sync_enable"
              :loading="syncing"
              @click="triggerSync"
            >
              <i class="fas fa-sync" />
              <span class="ml-2">{{ $t('steam.sync_now') }}</span>
            </n-button>
            <n-button size="small" strong :loading="refreshing" @click="refreshStatus">
              <i class="fas fa-rotate" />
              <span class="ml-2">{{ $t('steam.refresh_status') }}</span>
            </n-button>
          </div>
          <n-text v-if="!config.steam_sync_enable" depth="3" class="steam-help">
            {{ $t('steam.sync_now_disabled_hint') }}
          </n-text>
        </n-space>
      </n-card>
    </section>

    <section v-if="platform === 'linux'" class="steam-section">
      <n-divider title-placement="left" class="steam-section-divider">
        <n-text depth="3" strong>{{ $t('steam.settings_title') }}</n-text>
      </n-divider>

      <n-card class="steam-card" :segmented="{ content: true }">
        <n-form label-placement="top" :show-feedback="false">
          <div class="settings-grid">
            <div class="md:col-span-2">
              <Checkbox
                v-model="config.steam_sync_enable"
                id="steam_sync_enable"
                :default="store.defaults.steam_sync_enable"
                :localePrefix="'steam'"
                label="steam.enable"
                desc="steam.enable_desc"
              />
            </div>
            <div class="md:col-span-2">
              <Checkbox
                v-model="config.steam_sync_auto_sync"
                id="steam_sync_auto_sync"
                :default="store.defaults.steam_sync_auto_sync"
                :localePrefix="'steam'"
                label="steam.auto_sync"
                desc="steam.auto_sync_desc"
                :disabled="!config.steam_sync_enable"
              />
            </div>
            <div>
              <n-form-item :label="$t('steam.delete_after_days')">
                <n-input-number
                  id="steam_sync_autosync_delete_after_days"
                  v-model:value="config.steam_sync_autosync_delete_after_days"
                  :min="0"
                  :max="3650"
                  :show-button="true"
                  class="steam-number-input"
                  :disabled="!config.steam_sync_enable"
                />
              </n-form-item>
              <n-text depth="3" class="steam-help">
                {{ $t('steam.delete_after_days_desc') }} (0 = {{ $t('_common.disabled') }})
              </n-text>
            </div>
            <div>
              <Checkbox
                v-model="config.steam_sync_autosync_remove_uninstalled"
                id="steam_sync_autosync_remove_uninstalled"
                :default="store.defaults.steam_sync_autosync_remove_uninstalled"
                :localePrefix="'steam'"
                label="steam.remove_uninstalled"
                desc="steam.remove_uninstalled_desc"
                :disabled="!config.steam_sync_enable"
              />
            </div>
          </div>
        </n-form>
      </n-card>

      <n-card class="steam-card" :segmented="{ content: true }">
        <template #header>
          <div class="steam-card-heading">
            <n-icon size="18"><i class="fas fa-broom" /></n-icon>
            <n-text strong>{{ $t('steam.maintenance_title') }}</n-text>
          </div>
        </template>
        <n-space vertical size="small">
          <n-text depth="3" class="steam-help">{{ $t('steam.delete_all_autosync_desc') }}</n-text>
          <div>
            <n-button
              size="small"
              type="error"
              strong
              :loading="deleting"
              @click="openDeleteConfirm"
            >
              <i class="fas fa-trash" />
              <span class="ml-2">{{ $t('steam.delete_all_autosync') }}</span>
            </n-button>
          </div>
        </n-space>
      </n-card>
    </section>
  </n-space>

  <n-modal :show="showDeleteConfirm" @update:show="(v) => (showDeleteConfirm = v)">
    <n-card :bordered="false" style="max-width: 32rem; width: 100%">
      <template #header>
        <div class="flex items-center gap-2">
          <i class="fas fa-trash" />
          <span>{{ $t('steam.delete_all_autosync_confirm_title') }}</span>
        </div>
      </template>
      <div class="text-sm">
        {{ $t('steam.delete_all_autosync_confirm_body') }}
      </div>
      <template #footer>
        <div class="w-full flex items-center justify-center gap-3">
          <n-button type="default" strong @click="showDeleteConfirm = false">{{
            $t('_common.cancel')
          }}</n-button>
          <n-button type="error" strong :loading="deleting" @click="confirmDeleteAll">{{
            $t('_common.continue')
          }}</n-button>
        </div>
      </template>
    </n-card>
  </n-modal>
</template>

<script setup lang="ts">
import { computed, onMounted, reactive, ref } from 'vue';
import {
  NInputNumber,
  NButton,
  NAlert,
  NTag,
  NModal,
  NCard,
  NDivider,
  NForm,
  NFormItem,
  NIcon,
  NSpace,
  NText,
  useNotification,
} from 'naive-ui';
import { useI18n } from 'vue-i18n';
import Checkbox from '@/Checkbox.vue';
import { useConfigStore } from '@/stores/config';
import { storeToRefs } from 'pinia';
import { http } from '@/http';

const store = useConfigStore();
const { config, metadata } = storeToRefs(store);
const platform = computed(() =>
  (metadata.value?.platform || config.value?.platform || '').toLowerCase(),
);
const { t } = useI18n();

const notification = useNotification();
function notify(type: 'success' | 'error' | 'info' | 'warning', content: string) {
  notification.create({ type, content, duration: 5000 });
}

const status = reactive<{ installedGamesFound?: number }>({});
const syncing = ref(false);
const refreshing = ref(false);
const deleting = ref(false);
const showDeleteConfirm = ref(false);

const gamesFoundText = computed(() => {
  if (status.installedGamesFound === undefined) return t('steam.games_found_unknown');
  return t('steam.games_found', { count: status.installedGamesFound });
});

interface SteamStatusResponse {
  installed_games_found?: number;
}
interface SteamSyncResponse {
  status?: boolean;
  added?: number;
  updated?: number;
  removed?: number;
}
interface PurgeAutosyncResponse {
  status?: boolean;
}

async function refreshStatus() {
  if (platform.value !== 'linux') return;
  refreshing.value = true;
  try {
    const r = await http.get<SteamStatusResponse>('/api/steam/status', {
      validateStatus: () => true,
    });
    if (r.status === 200 && r.data) {
      status.installedGamesFound = Number(r.data.installed_games_found ?? 0);
    }
  } catch {
    // leave prior status displayed
  }
  refreshing.value = false;
}

async function triggerSync() {
  if (!config.value.steam_sync_enable) return;
  syncing.value = true;
  try {
    // The Enable toggle is saved via a debounced auto-save patch; flush it now so the
    // daemon's in-process config::steam_sync.enable is current before we hit the sync
    // endpoint, which 400s if it's still stale/disabled.
    if (store.hasPendingPatch()) {
      await store.flushPatchQueue();
    }
    const r = await http.post<SteamSyncResponse>(
      '/api/steam/sync',
      {},
      { validateStatus: () => true },
    );
    if (r.status >= 200 && r.status < 300 && r.data?.status === true) {
      notify(
        'success',
        t('steam.sync_success', {
          added: r.data.added ?? 0,
          updated: r.data.updated ?? 0,
          removed: r.data.removed ?? 0,
        }),
      );
      await refreshStatus();
    } else {
      notify('error', t('steam.sync_error'));
    }
  } catch {
    notify('error', t('steam.sync_error'));
  }
  syncing.value = false;
}

function openDeleteConfirm() {
  showDeleteConfirm.value = true;
}

async function confirmDeleteAll() {
  deleting.value = true;
  try {
    const r = await http.post<PurgeAutosyncResponse>(
      '/api/apps/purge_autosync',
      {},
      { validateStatus: () => true },
    );
    if (r.status >= 200 && r.status < 300 && r.data?.status === true) {
      notify('success', t('steam.delete_all_autosync_success'));
      showDeleteConfirm.value = false;
    } else {
      notify('error', t('steam.delete_all_autosync_error'));
    }
  } catch {
    notify('error', t('steam.delete_all_autosync_error'));
  }
  deleting.value = false;
}

onMounted(async () => {
  if (!config.value) await store.fetchConfig();
  await refreshStatus();
});
</script>

<style scoped>
.steam-tab {
  min-width: 0;
}

.steam-tab :deep(> .n-space-item) {
  width: 100%;
  min-width: 0;
}

.steam-section {
  display: flex;
  min-width: 0;
  width: 100%;
  flex-direction: column;
  gap: 1rem;
}

.steam-card {
  min-width: 0;
  width: 100%;
}

.steam-card-heading {
  display: flex;
  min-width: 0;
  align-items: center;
  gap: 0.625rem;
  line-height: 1.25rem;
}

.steam-status-line {
  display: flex;
  min-width: 0;
  align-items: center;
  gap: 0.5rem;
  font-size: 0.875rem;
}

.steam-actions {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: 0.5rem;
}

.settings-grid {
  display: grid;
  grid-template-columns: repeat(2, minmax(0, 1fr));
  align-items: start;
  gap: 1.5rem 2rem;
}

.settings-grid > * {
  min-width: 0;
}

.steam-number-input {
  width: 100%;
  max-width: 10rem;
}

.steam-help {
  display: block;
  font-size: 0.75rem;
  line-height: 1.45;
  margin-top: 0.375rem;
}

@media (max-width: 767px) {
  .settings-grid {
    grid-template-columns: minmax(0, 1fr);
  }
}
</style>
