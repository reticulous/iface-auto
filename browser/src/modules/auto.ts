import { useMenuStore } from 'spangap-browser/stores/menu'
import AutoPanel from '../panels/AutoPanel.vue'

export function registerAuto() {
  useMenuStore().register('settings', 'Settings', 10, [
    {
      id: 'reticulum', label: 'Reticulum', type: 'submenu', order: 30,
      children: [
        {
          id: 'reticulum.transports', label: 'Transports', type: 'submenu', order: 20,
          children: [
            { id: 'reticulum.transports.auto', label: 'AutoInterface', type: 'panel', order: 5,
              component: AutoPanel },
          ],
        },
      ],
    },
  ])
}
