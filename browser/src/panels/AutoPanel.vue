<template>
  <div class="q-gutter-y-md">
    <PanelHeading title="AutoInterface" />

    <div class="text-caption" style="opacity:0.7">
      Zero-configuration RNS over the local network. Peers on the same
      WiFi link are discovered automatically with IPv6 link-local
      multicast — no addresses to configure — and RNS packets travel
      over UDP. Wire-compatible with desktop Reticulum's AutoInterface.
      Requires WiFi to be up.
    </div>

    <SettingToggle label="Enabled" k="s.auto.enable" />

    <SettingText label="Group name" k="s.auto.group" />
    <div class="text-caption" style="opacity:0.6; margin-top:-8px">
      Only nodes sharing this name peer with each other (default
      “reticulum”). Changing it forms a separate network.
    </div>
    <SettingSelect label="Interface mode" k="s.auto.mode" :options="modeOptions" />

    <q-separator dark class="q-mt-md" />

    <div class="row items-center no-wrap">
      <div class="col-4 text-caption">State</div>
      <div class="col">
        <q-badge v-if="state === 'up'"                  color="green">up</q-badge>
        <q-badge v-else-if="state === 'waiting_wifi'"   color="orange">waiting wifi</q-badge>
        <q-badge v-else-if="state === 'waiting_addr'"   color="orange">waiting address</q-badge>
        <q-badge v-else-if="state === 'rnsd_unavailable'" color="red">rnsd unavailable</q-badge>
        <q-badge v-else-if="state === 'error'"          color="red">error</q-badge>
        <q-badge v-else                                 color="grey">{{ state || 'down' }}</q-badge>
      </div>
    </div>

    <div v-if="state === 'up'" class="row items-center no-wrap">
      <div class="col-4 text-caption">Address</div>
      <div class="col text-caption" style="word-break:break-all">{{ addr }}</div>
    </div>
    <div class="row items-center no-wrap">
      <div class="col-4 text-caption">Peers</div>
      <div class="col text-caption">{{ peers }}</div>
    </div>
    <div class="row items-center no-wrap">
      <div class="col-4 text-caption">Packets</div>
      <div class="col text-caption">
        rx {{ rxPackets }} (drop {{ rxDrop }}) · tx {{ txPackets }} (fail {{ txFail }})
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { computed } from 'vue'
import { useDeviceStore } from 'diptych-browser/stores/device'

const device = useDeviceStore()

const state     = computed(() => String(device.get('auto.state') ?? ''))
const addr      = computed(() => String(device.get('auto.addr') ?? ''))
const peers     = computed(() => Number(device.get('auto.peers') ?? 0))
const rxPackets = computed(() => Number(device.get('auto.stats.rx_packets') ?? 0))
const txPackets = computed(() => Number(device.get('auto.stats.tx_packets') ?? 0))
const rxDrop    = computed(() => Number(device.get('auto.stats.rx_drop') ?? 0))
const txFail    = computed(() => Number(device.get('auto.stats.tx_fail') ?? 0))

const modeOptions = [
  { label: 'Gateway',      value: 'gateway' },
  { label: 'Full',         value: 'full' },
  { label: 'Access point', value: 'access_point' },
  { label: 'Roaming',      value: 'roaming' },
  { label: 'Boundary',     value: 'boundary' },
]
</script>
