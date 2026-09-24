targetScope = 'resourceGroup'

param location string
@minLength(9)
@maxLength(40)
param runId string
param expiresAt string
@secure()
param adminPublicKey string
param adminUsername string = 'benchmark'
@description('Concrete marketplace image version; latest is rejected by the controller.')
param imageVersion string
param imagePublisher string = 'Canonical'
param imageOffer string = 'ubuntu-24_04-lts'
param imageSku string = 'server'
@allowed(['Standard_D2s_v6'])
param vmSize string = 'Standard_D2s_v6'

var tags = {
  'simdurl-owner': 'simdurl'
  'simdurl-run': runId
  'simdurl-expires-at': expiresAt
}

resource network 'Microsoft.Network/virtualNetworks@2024-05-01' existing = {
  name: 'simdurl-bench-vnet'
}

resource publicIp 'Microsoft.Network/publicIPAddresses@2024-10-01' = {
  name: '${runId}-ip'
  location: location
  tags: tags
  sku: { name: 'Standard' }
  properties: {
    publicIPAllocationMethod: 'Static'
    publicIPAddressVersion: 'IPv4'
  }
}

resource nic 'Microsoft.Network/networkInterfaces@2024-10-01' = {
  name: '${runId}-nic'
  location: location
  tags: tags
  properties: {
    enableAcceleratedNetworking: false
    ipConfigurations: [
      {
        name: 'primary'
        properties: {
          privateIPAllocationMethod: 'Dynamic'
          subnet: { id: '${network.id}/subnets/benchmark' }
          publicIPAddress: {
            id: publicIp.id
            properties: { deleteOption: 'Delete' }
          }
        }
      }
    ]
  }
}

resource vm 'Microsoft.Compute/virtualMachines@2024-07-01' = {
  name: '${runId}-vm'
  location: location
  tags: tags
  properties: {
    hardwareProfile: { vmSize: vmSize }
    storageProfile: {
      imageReference: {
        publisher: imagePublisher
        offer: imageOffer
        sku: imageSku
        version: imageVersion
      }
      osDisk: {
        name: '${runId}-disk'
        createOption: 'FromImage'
        deleteOption: 'Delete'
        diskSizeGB: 64
        caching: 'ReadWrite'
        managedDisk: { storageAccountType: 'StandardSSD_LRS' }
      }
    }
    osProfile: {
      computerName: '${runId}-vm'
      adminUsername: adminUsername
      customData: base64(loadTextContent('cloud-init.yaml'))
      linuxConfiguration: {
        disablePasswordAuthentication: true
        provisionVMAgent: true
        patchSettings: {
          patchMode: 'ImageDefault'
          assessmentMode: 'ImageDefault'
        }
        ssh: {
          publicKeys: [{ path: '/home/${adminUsername}/.ssh/authorized_keys', keyData: adminPublicKey }]
        }
      }
    }
    networkProfile: {
      networkInterfaces: [{ id: nic.id, properties: { primary: true, deleteOption: 'Delete' } }]
    }
    diagnosticsProfile: { bootDiagnostics: { enabled: true } }
  }
}

output vmId string = vm.id
output vmName string = vm.name
output diskId string = resourceId('Microsoft.Compute/disks', '${runId}-disk')
output nicId string = nic.id
output publicIpId string = publicIp.id
