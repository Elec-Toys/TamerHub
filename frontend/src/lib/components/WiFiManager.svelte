<script lang="ts">
  import { hubState } from '$lib/stores';
  import { WifiScanStatus } from '$lib/_fbs/open-shock/serialization/types/wifi-scan-status';
  import {
    disconnectWifiNetwork,
    setApEnabled,
    setWifiEnabled,
    startWifiScan,
    stopWifiScan,
  } from '$lib/api';
  import AddHiddenNetworkDialog from '$lib/components/AddHiddenNetworkDialog.svelte';
  import WiFiEntry from '$lib/components/WiFiEntry.svelte';
  import { Button } from '$lib/components/ui/button';
  import ScrollArea from '$lib/components/ui/scroll-area/scroll-area.svelte';
  import { LoaderCircle, Radar, Radio, Shield, Wifi, WifiOff } from '@lucide/svelte';

  let wifiEnabled = $derived(hubState.wifiStaEnabled ?? false);
  let apEnabled = $derived(hubState.wifiApEnabled ?? false);
  let scanStatus = $derived(hubState.wifiScanStatus);
  let isScanning = $derived(
    scanStatus === WifiScanStatus.Started || scanStatus === WifiScanStatus.InProgress
  );
  let connectedBSSID = $derived(hubState.wifiConnectedBSSID);

  let strengthSortedGroups = $derived(
    Array.from(hubState.wifiNetworkGroups.entries()).sort(
      (a, b) => b[1].networks[0].rssi - a[1].networks[0].rssi
    )
  );

  let savedGroups = $derived(strengthSortedGroups.filter(([, group]) => group.saved));
  let savedOnlySSIDs = $derived(hubState.savedOnlySSIDs);
  let availableGroups = $derived(strengthSortedGroups.filter(([, group]) => !group.saved));

  let connectedNetwork = $derived.by(() => {
    if (!connectedBSSID) return null;

    for (const [, group] of strengthSortedGroups) {
      if (group.networks.some((network) => network.bssid === connectedBSSID)) {
        return group;
      }
    }

    return null;
  });
  let hasConnectedBSSID = $derived(connectedBSSID != null);

  async function toggleWifi() {
    await setWifiEnabled(!wifiEnabled);
  }

  async function toggleAccessPoint() {
    await setApEnabled(!apEnabled);
  }

  async function wifiScan() {
    if (isScanning) {
      await stopWifiScan();
      return;
    }

    await startWifiScan();
  }

  function wifiDisconnect() {
    disconnectWifiNetwork();
  }
</script>

<div class="flex flex-col gap-4">
  <div class="rounded-lg border p-3">
    <div class="mb-1 flex items-center gap-2">
      {#if hasConnectedBSSID}
        <Wifi class="h-5 w-5 text-green-500" />
      {:else if wifiEnabled}
        <WifiOff class="h-5 w-5 text-yellow-500" />
      {:else}
        <WifiOff class="text-muted-foreground h-5 w-5" />
      {/if}
      <p class="text-sm font-medium">Status</p>
    </div>

    {#if hasConnectedBSSID}
      <p class="text-sm">Connected to {connectedNetwork?.ssid || 'Network'}</p>
      <p class="text-muted-foreground text-xs">
        Signal: {connectedNetwork?.networks[0]?.rssi ?? '?'} dBm
      </p>
    {:else if wifiEnabled}
      <p class="text-sm">WiFi is enabled but not connected.</p>
    {:else}
      <p class="text-sm">WiFi is disabled.</p>
    {/if}
  </div>

  <label class="flex cursor-pointer items-center justify-between rounded-lg border p-3">
    <div class="flex items-center gap-2">
      {#if wifiEnabled}
        <Wifi class="h-5 w-5 text-green-500" />
      {:else}
        <WifiOff class="text-muted-foreground h-5 w-5" />
      {/if}
      <div>
        <p class="text-sm font-medium">WiFi</p>
        <p class="text-muted-foreground text-xs">Connect to a local wireless network</p>
      </div>
    </div>
    <input type="checkbox" checked={wifiEnabled} onchange={toggleWifi} class="h-4 w-4" />
  </label>

  <label class="flex cursor-pointer items-center justify-between rounded-lg border p-3">
    <div class="flex items-center gap-2">
      <Radio class={`h-5 w-5 ${apEnabled ? 'text-blue-500' : 'text-muted-foreground'}`} />
      <div>
        <p class="text-sm font-medium">Access Point</p>
        <p class="text-muted-foreground text-xs">Broadcast the setup hotspot</p>
      </div>
    </div>
    <input type="checkbox" checked={apEnabled} onchange={toggleAccessPoint} class="h-4 w-4" />
  </label>

  {#if wifiEnabled}
    <div class="rounded-lg border p-3">
      <div class="mb-2 flex items-center justify-between">
        <div class="flex items-center gap-2">
          <Shield class="h-5 w-5 text-blue-500" />
          <div>
            <p class="text-sm font-medium">Connect</p>
            <p class="text-muted-foreground text-xs">Scan and manage saved networks</p>
          </div>
        </div>
        <div class="flex items-center gap-2">
          <AddHiddenNetworkDialog />
          <Button
            variant="outline"
            size="icon"
            onclick={wifiScan}
            title={isScanning ? 'Stop scan' : 'Scan'}
          >
            {#if isScanning}
              <LoaderCircle class="h-4 w-4 animate-spin" />
            {:else}
              <Radar class="h-4 w-4" />
            {/if}
          </Button>
        </div>
      </div>

      {#if hasConnectedBSSID}
        <div
          class="mb-3 flex items-center justify-between rounded-lg border border-green-500/30 bg-green-500/10 p-3"
        >
          <div>
            <p class="text-sm font-medium">Active connection</p>
            <p class="text-muted-foreground text-xs">{connectedNetwork?.ssid || 'Network'}</p>
          </div>
          <Button variant="outline" size="sm" onclick={wifiDisconnect}>Disconnect</Button>
        </div>
      {/if}

      {#if savedGroups.length > 0 || savedOnlySSIDs.length > 0}
        <div class="mb-3">
          <h4 class="text-muted-foreground mb-2 text-sm font-medium">Saved Networks</h4>
          {#each savedGroups as [key, group] (key)}
            <WiFiEntry ssid={group.ssid} netgroup={group} />
          {/each}
          {#each savedOnlySSIDs as ssid (ssid)}
            <WiFiEntry {ssid} />
          {/each}
        </div>
      {/if}

      <div>
        <h4 class="text-muted-foreground mb-2 text-sm font-medium">Available Networks</h4>
        <ScrollArea class="h-52">
          {#if availableGroups.length > 0}
            {#each availableGroups as [key, group] (key)}
              <WiFiEntry ssid={group.ssid} netgroup={group} />
            {/each}
          {:else if isScanning}
            <p class="text-muted-foreground py-4 text-center text-sm">Scanning...</p>
          {:else}
            <p class="text-muted-foreground py-4 text-center text-sm">
              No networks found. Tap scan to search.
            </p>
          {/if}
        </ScrollArea>
      </div>
    </div>
  {:else}
    <p class="text-muted-foreground py-4 text-center text-sm">
      Enable WiFi to scan and connect to networks.
    </p>
  {/if}
</div>
