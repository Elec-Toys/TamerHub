<script lang="ts">
  import {
    setOtaDomain,
    setOtaAllowBackendManagement,
    setOtaRequireManualApproval,
    getOtaSettings,
    installOtaUpdate,
  } from '$lib/api';
  import { Button } from '$lib/components/ui/button';
  import { Input } from '$lib/components/ui/input';
  import { Label } from '$lib/components/ui/label';
  import {
    Dialog,
    DialogContent,
    DialogHeader,
    DialogTitle,
    DialogDescription,
  } from '$lib/components/ui/dialog';
  import { getApiBaseUrl } from '$lib/utils/localRedirect';
  import { Info, Upload } from '@lucide/svelte';
  import { onMount } from 'svelte';

  const GITHUB_PREFIX = 'https://github.com/';

  // ── Live settings fetched from the device ──────────────────────────────────
  let githubSource = $state('');
  let allowBackendManagement = $state(false);
  let promptToUpdate = $state(true);
  let settingsLoaded = $state(false);

  async function loadSettings() {
    const s = await getOtaSettings();
    if (s) {
      githubSource = GITHUB_PREFIX + s.repoSlug;
      allowBackendManagement = s.allowBackendManagement;
      promptToUpdate = s.promptUpdates;
      settingsLoaded = true;
    }
  }

  onMount(loadSettings);

  // ── GitHub Source save ─────────────────────────────────────────────────────
  let saveLinkStatus = $state<'idle' | 'saving' | 'saved' | 'error'>('idle');

  async function saveGitHubSource() {
    let slug = githubSource.trim();
    if (slug.startsWith(GITHUB_PREFIX)) slug = slug.slice(GITHUB_PREFIX.length);
    if (!slug) return;

    saveLinkStatus = 'saving';
    try {
      await setOtaDomain(GITHUB_PREFIX + slug);
      saveLinkStatus = 'saved';
    } catch {
      saveLinkStatus = 'error';
    }
    setTimeout(() => (saveLinkStatus = 'idle'), 3000);
  }

  // ── Toggle handlers ────────────────────────────────────────────────────────
  async function toggleBackendManagement() {
    const next = !allowBackendManagement;
    await setOtaAllowBackendManagement(next);
    allowBackendManagement = next;
  }

  async function togglePromptToUpdate() {
    const next = !promptToUpdate;
    await setOtaRequireManualApproval(next);
    promptToUpdate = next;
  }

  // ── Check for Updates ─────────────────────────────────────────────────────
  type CheckState = 'idle' | 'checking' | 'upToDate' | 'updateAvailable' | 'failed';
  let checkState = $state<CheckState>('idle');
  let checkError = $state('');
  let availableVersion = $state('');
  let currentVersion = $state('');
  let updateDialogOpen = $state(false);
  let updateStarted = $state(false);

  async function checkForUpdates() {
    checkState = 'checking';
    checkError = '';
    availableVersion = '';
    currentVersion = '';
    updateStarted = false;

    try {
      // Resolve the repo slug from current githubSource for the GitHub API call
      let slug = githubSource.trim();
      if (slug.startsWith(GITHUB_PREFIX)) slug = slug.slice(GITHUB_PREFIX.length);

      const [deviceRes, githubRes] = await Promise.allSettled([
        fetch(getApiBaseUrl() + '/api/version'),
        fetch(`https://api.github.com/repos/${slug}/releases/latest`, {
          headers: { Accept: 'application/vnd.github+json' },
        }),
      ]);

      if (deviceRes.status !== 'fulfilled' || !deviceRes.value.ok) {
        checkState = 'failed';
        checkError = 'Could not reach device';
        return;
      }
      if (githubRes.status !== 'fulfilled' || !githubRes.value.ok) {
        checkState = 'failed';
        checkError = 'Could not reach GitHub';
        return;
      }

      const devData = await deviceRes.value.json();
      const ghData = await githubRes.value.json();

      currentVersion = devData.version ?? '';
      const tag: string = ghData.tag_name ?? '';
      availableVersion = tag.startsWith('v') ? tag.slice(1) : tag;

      if (!currentVersion || !availableVersion) {
        checkState = 'failed';
        checkError = 'Could not parse version info';
        return;
      }

      if (currentVersion === availableVersion) {
        checkState = 'upToDate';
      } else {
        checkState = 'updateAvailable';
        updateDialogOpen = true;
      }
    } catch {
      checkState = 'failed';
      checkError = 'Network error';
    }
  }

  async function confirmInstall() {
    updateStarted = true;
    checkState = 'idle';
    updateDialogOpen = false;
    const ok = await installOtaUpdate(availableVersion);
    if (!ok) {
      checkError = 'Failed to start update — check device logs';
      checkState = 'failed';
      updateStarted = false;
    }
  }

  // ── Version Info dialog ───────────────────────────────────────────────────
  let versionDialogOpen = $state(false);
  let vDialogCurrentVersion = $state<string | null>(null);
  let vDialogLatestVersion = $state<string | null>(null);
  let vDialogError = $state<string | null>(null);
  let vDialogLoading = $state(false);

  async function openVersionDialog() {
    versionDialogOpen = true;
    vDialogLoading = true;
    vDialogError = null;
    vDialogCurrentVersion = null;
    vDialogLatestVersion = null;

    let slug = githubSource.trim();
    if (slug.startsWith(GITHUB_PREFIX)) slug = slug.slice(GITHUB_PREFIX.length);

    try {
      const [deviceRes, githubRes] = await Promise.allSettled([
        fetch(getApiBaseUrl() + '/api/version'),
        fetch(`https://api.github.com/repos/${slug}/releases/latest`, {
          headers: { Accept: 'application/vnd.github+json' },
        }),
      ]);

      if (deviceRes.status === 'fulfilled' && deviceRes.value.ok) {
        const data = await deviceRes.value.json();
        vDialogCurrentVersion = data.version ?? 'Unknown';
      } else {
        vDialogCurrentVersion = 'Unavailable';
      }

      if (githubRes.status === 'fulfilled' && githubRes.value.ok) {
        const data = await githubRes.value.json();
        const tag: string = data.tag_name ?? '';
        vDialogLatestVersion = tag.startsWith('v') ? tag.slice(1) : tag || 'Unavailable';
      } else {
        vDialogLatestVersion = 'Unavailable (no internet?)';
      }
    } catch {
      vDialogError = 'Failed to fetch version info';
    } finally {
      vDialogLoading = false;
    }
  }

  // ── Local file upload (drag-and-drop flash) ────────────────────────────────
  let fileInput: HTMLInputElement;
  let selectedFile = $state<File | null>(null);
  let isDragOver = $state(false);
  type UploadStatus = 'idle' | 'uploading' | 'success' | 'error';
  let uploadStatus = $state<UploadStatus>('idle');
  let uploadError = $state('');

  function formatBytes(n: number): string {
    if (n < 1024) return `${n} B`;
    if (n < 1048576) return `${(n / 1024).toFixed(1)} KB`;
    return `${(n / 1048576).toFixed(2)} MB`;
  }

  function handleFileSelect(e: Event) {
    const input = e.target as HTMLInputElement;
    selectedFile = input.files?.[0] ?? null;
    uploadStatus = 'idle';
    uploadError = '';
  }

  function handleDrop(e: DragEvent) {
    e.preventDefault();
    isDragOver = false;
    const f = e.dataTransfer?.files[0];
    if (f) {
      selectedFile = f;
      uploadStatus = 'idle';
      uploadError = '';
    }
  }

  function isKnownOtaFile(name: string): boolean {
    return name === 'app.bin' || name.endsWith('staticfs.bin') || name.endsWith('littlefs.bin');
  }

  async function uploadFile() {
    if (!selectedFile) return;

    if (!isKnownOtaFile(selectedFile.name)) {
      uploadError = `Unexpected filename "${selectedFile.name}". OTA only accepts app.bin (firmware) or staticfs.bin/littlefs.bin (filesystem). A full flash image will fail.`;
      uploadStatus = 'error';
      return;
    }

    uploadStatus = 'uploading';
    uploadError = '';
    try {
      const isFs = selectedFile.name.endsWith('staticfs.bin') || selectedFile.name.endsWith('littlefs.bin');
      const res = await fetch(getApiBaseUrl() + '/api/ota/upload?type=' + (isFs ? 'fs' : 'app'), {
        method: 'POST',
        headers: { 'Content-Type': 'application/octet-stream' },
        body: selectedFile,
      });
      if (res.ok) {
        uploadStatus = 'success';
        selectedFile = null;
      } else {
        const j = await res.json().catch(() => ({}));
        uploadError = (j as { error?: string }).error ?? `HTTP ${res.status}`;
        uploadStatus = 'error';
      }
    } catch (err) {
      uploadError = 'Network error — check device connection';
      uploadStatus = 'error';
    }
  }
</script>

<div class="flex flex-col gap-4">
  <div>
    <h3 class="text-lg font-semibold">OTA Updates</h3>
    <p class="text-muted-foreground text-sm">Over-the-air firmware update settings.</p>
  </div>

  <div class="flex flex-col gap-4">
    <!-- GitHub Source -->
    <div class="flex flex-col gap-2">
      <Label for="ota-source">GitHub Source</Label>
      <p class="text-muted-foreground text-xs">
        Full GitHub repository URL used to fetch firmware releases.
      </p>
      <div class="flex gap-2">
        <Input
          id="ota-source"
          type="text"
          bind:value={githubSource}
          placeholder="https://github.com/owner/repo"
          disabled={!settingsLoaded}
        />
        <Button size="sm" onclick={saveGitHubSource} disabled={saveLinkStatus === 'saving'}>
          {saveLinkStatus === 'saving' ? 'Saving…' : 'Save'}
        </Button>
      </div>
      {#if saveLinkStatus === 'saved'}
        <p class="text-sm text-green-600 dark:text-green-400">✓ Saved on device</p>
      {:else if saveLinkStatus === 'error'}
        <p class="text-sm text-red-500">Failed to save — check connection</p>
      {/if}
    </div>

    <!-- Allow Backend Management toggle -->
    <label class="flex cursor-pointer items-center justify-between rounded-lg border p-3">
      <div>
        <p class="text-sm font-medium">Allow Backend Management</p>
        <p class="text-muted-foreground text-xs">
          Let the gateway server trigger an automatic GitHub update check.
        </p>
      </div>
      <input
        type="checkbox"
        checked={allowBackendManagement}
        onchange={toggleBackendManagement}
        disabled={!settingsLoaded}
        class="h-4 w-4"
      />
    </label>

    <!-- Prompt To Update toggle -->
    <label class="flex cursor-pointer items-center justify-between rounded-lg border p-3">
      <div>
        <p class="text-sm font-medium">Prompt To Update</p>
        <p class="text-muted-foreground text-xs">
          Show an approval prompt on the device before installing an update.
        </p>
      </div>
      <input
        type="checkbox"
        checked={promptToUpdate}
        onchange={togglePromptToUpdate}
        disabled={!settingsLoaded}
        class="h-4 w-4"
      />
    </label>

    <!-- Check for Updates -->
    <div class="flex flex-col gap-2">
      <Button onclick={checkForUpdates} disabled={checkState === 'checking' || updateStarted}>
        {checkState === 'checking' ? 'Checking…' : 'Check for Updates'}
      </Button>
      {#if checkState === 'upToDate'}
        <p class="text-center text-sm text-green-600 dark:text-green-400">
          ✓ Up to date (v{currentVersion})
        </p>
      {:else if checkState === 'failed'}
        <p class="text-center text-sm text-red-500">
          Failed to check: {checkError}
        </p>
      {:else if updateStarted}
        <p class="text-center text-sm text-blue-600 dark:text-blue-400">
          Update started — device will reboot when done.
        </p>
      {/if}
    </div>

    <!-- Version Info -->
    <Button variant="outline" onclick={openVersionDialog} class="w-full">
      <Info class="mr-2 h-4 w-4" />
      Version Info
    </Button>

    <hr class="border-border" />

    <!-- Local Firmware Upload -->
    <div class="flex flex-col gap-2">
      <h4 class="text-sm font-semibold">Manual Firmware Upload</h4>
      <p class="text-muted-foreground text-xs">
        Flash a local <code>.bin</code> file directly. Accepts <code>app.bin</code> (firmware) or
        <code>staticfs.bin</code> (filesystem). The device will reboot automatically after flashing.
      </p>

      <!-- Drop zone -->
      <input
        type="file"
        class="hidden"
        bind:this={fileInput}
        onchange={handleFileSelect}
      />
      <!-- svelte-ignore a11y_click_events_have_key_events a11y_no_static_element_interactions -->
      <div
        class="flex cursor-pointer flex-col items-center justify-center gap-2 rounded-lg border-2 border-dashed p-6 text-center transition-colors
          {isDragOver ? 'border-primary bg-primary/5' : 'border-border hover:border-primary/50 hover:bg-muted/30'}"
        onclick={() => fileInput.click()}
        ondragover={(e) => { e.preventDefault(); isDragOver = true; }}
        ondragleave={() => (isDragOver = false)}
        ondrop={handleDrop}
        role="button"
        tabindex="0"
      >
        {#if selectedFile}
          <Upload class="text-primary h-6 w-6" />
          <p class="text-sm font-medium">{selectedFile.name}</p>
          <p class="text-muted-foreground text-xs">{formatBytes(selectedFile.size)}</p>
        {:else}
          <Upload class="text-muted-foreground h-6 w-6" />
          <p class="text-sm font-medium">Drop .bin file here</p>
          <p class="text-muted-foreground text-xs">or click to browse</p>
        {/if}
      </div>

      {#if selectedFile && uploadStatus !== 'success'}
        <Button
          onclick={uploadFile}
          disabled={uploadStatus === 'uploading'}
          class="w-full"
        >
          {uploadStatus === 'uploading' ? 'Flashing… please wait' : 'Upload & Flash'}
        </Button>
      {/if}

      {#if uploadStatus === 'uploading'}
        <p class="text-muted-foreground text-center text-xs">
          Uploading and writing to flash — this may take a moment…
        </p>
      {:else if uploadStatus === 'success'}
        <p class="text-center text-sm text-green-600 dark:text-green-400">
          ✓ Flash complete! Device is rebooting…
        </p>
      {:else if uploadStatus === 'error'}
        <p class="text-center text-sm text-red-500">Flash failed: {uploadError}</p>
      {/if}
    </div>
  </div>
</div>

<!-- Update Available dialog -->
<Dialog bind:open={updateDialogOpen}>
  <DialogContent class="sm:max-w-[380px]">
    <DialogHeader>
      <DialogTitle>Update Available</DialogTitle>
      <DialogDescription>
        v{availableVersion} is available. Your device is on v{currentVersion}.
      </DialogDescription>
    </DialogHeader>
    <div class="flex flex-col gap-3 pt-2">
      <Button size="lg" class="w-full" onclick={confirmInstall}>Install Now</Button>
      <Button size="lg" variant="outline" class="w-full" onclick={() => { updateDialogOpen = false; checkState = 'idle'; }}>
        Not Now
      </Button>
    </div>
  </DialogContent>
</Dialog>

<!-- Version Info dialog -->
<Dialog bind:open={versionDialogOpen}>
  <DialogContent class="sm:max-w-[380px]">
    <DialogHeader>
      <DialogTitle>Firmware Version Info</DialogTitle>
    </DialogHeader>
    {#if vDialogLoading}
      <p class="text-muted-foreground text-sm">Fetching version info…</p>
    {:else if vDialogError}
      <p class="text-sm text-red-500">{vDialogError}</p>
    {:else}
      <div class="flex flex-col gap-3">
        <div class="flex items-center justify-between rounded-lg border p-3">
          <span class="text-sm font-medium">Installed</span>
          <span class="font-mono text-sm">{vDialogCurrentVersion ?? '…'}</span>
        </div>
        <div class="flex items-center justify-between rounded-lg border p-3">
          <span class="text-sm font-medium">Latest (GitHub)</span>
          <span class="font-mono text-sm">{vDialogLatestVersion ?? '…'}</span>
        </div>
        {#if vDialogCurrentVersion && vDialogLatestVersion && vDialogCurrentVersion !== 'Unavailable' && !vDialogLatestVersion.startsWith('Unavailable')}
          {#if vDialogCurrentVersion === vDialogLatestVersion}
            <p class="text-center text-sm text-green-600 dark:text-green-400">✓ Up to date</p>
          {:else}
            <p class="text-center text-sm text-yellow-600 dark:text-yellow-400">⬆ Update available</p>
          {/if}
        {/if}
      </div>
    {/if}
  </DialogContent>
</Dialog>
