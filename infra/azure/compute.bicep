targetScope = 'resourceGroup'

param location string = resourceGroup().location

// Fixed resources are deliberately excluded from every run manifest.
resource networkSecurityGroup 'Microsoft.Network/networkSecurityGroups@2024-10-01' = {
  name: 'simdurl-bench-nsg'
  location: location
  properties: {
    securityRules: [
      {
        name: 'DenyAllInbound'
        properties: {
          priority: 100
          access: 'Deny'
          direction: 'Inbound'
          protocol: '*'
          sourcePortRange: '*'
          destinationPortRange: '*'
          sourceAddressPrefix: '*'
          destinationAddressPrefix: '*'
        }
      }
    ]
  }
}

resource network 'Microsoft.Network/virtualNetworks@2024-10-01' = {
  name: 'simdurl-bench-vnet'
  location: location
  properties: {
    addressSpace: { addressPrefixes: ['10.83.0.0/24'] }
    subnets: [
      {
        name: 'benchmark'
        properties: {
          addressPrefix: '10.83.0.0/26'
          defaultOutboundAccess: false
          networkSecurityGroup: { id: networkSecurityGroup.id }
        }
      }
    ]
  }
}

output subnetId string = '${network.id}/subnets/benchmark'
