#! /bin/bash

# Instructions to allocate VMs on AWS

# NOTE: You need to add public addresses and associate it to your interfaces from the console
# TODO: add IP association as code

export AWS_REGION=us-west-2   # "us-west" on AWS usually means us-west-2 (Oregon)
export AZ=us-west-2a

export VPC_CIDR=10.0.0.0/16
export PUBLIC_SUBNET_CIDR=10.0.1.0/24
export CLUSTER_SUBNET_CIDR=10.0.2.0/24

export NAME=latency-2node
export KEY_NAME=latency-key   # must exist, or create it (see below)

aws ec2 create-key-pair \
  --key-name "$KEY_NAME" \
  --query 'KeyMaterial' \
  --output text > "${KEY_NAME}.pem"
chmod 400 "${KEY_NAME}.pem"

# VPC
VPC_ID=$(aws ec2 create-vpc --cidr-block "$VPC_CIDR" \
  --query 'Vpc.VpcId' --output text)
aws ec2 create-tags --resources "$VPC_ID" --tags Key=Name,Value="$NAME-vpc"

# Enable DNS hostnames (helps with public DNS)
aws ec2 modify-vpc-attribute --vpc-id "$VPC_ID" --enable-dns-hostnames

# Subnets
PUB_SUBNET_ID=$(aws ec2 create-subnet --vpc-id "$VPC_ID" --cidr-block "$PUBLIC_SUBNET_CIDR" --availability-zone "$AZ" \
  --query 'Subnet.SubnetId' --output text)
CL_SUBNET_ID=$(aws ec2 create-subnet --vpc-id "$VPC_ID" --cidr-block "$CLUSTER_SUBNET_CIDR" --availability-zone "$AZ" \
  --query 'Subnet.SubnetId' --output text)

aws ec2 create-tags --resources "$PUB_SUBNET_ID" --tags Key=Name,Value="$NAME-public"
aws ec2 create-tags --resources "$CL_SUBNET_ID" --tags Key=Name,Value="$NAME-cluster"

# Internet Gateway + attach
IGW_ID=$(aws ec2 create-internet-gateway --query 'InternetGateway.InternetGatewayId' --output text)
aws ec2 attach-internet-gateway --internet-gateway-id "$IGW_ID" --vpc-id "$VPC_ID"

# Public route table -> IGW (makes subnet public)
PUB_RT_ID=$(aws ec2 create-route-table --vpc-id "$VPC_ID" --query 'RouteTable.RouteTableId' --output text)
aws ec2 create-route --route-table-id "$PUB_RT_ID" --destination-cidr-block 0.0.0.0/0 --gateway-id "$IGW_ID"
aws ec2 associate-route-table --route-table-id "$PUB_RT_ID" --subnet-id "$PUB_SUBNET_ID"

# Auto-assign public IPv4 on the public subnet (so eth0 gets one)
aws ec2 modify-subnet-attribute --subnet-id "$PUB_SUBNET_ID" --map-public-ip-on-launch

SG_PUBLIC=$(aws ec2 create-security-group \
  --group-name "$NAME-sg-public" \
  --description "public access" \
  --vpc-id "$VPC_ID" \
  --query 'GroupId' --output text)

aws ec2 authorize-security-group-ingress --group-id "$SG_PUBLIC" \
  --ip-permissions '[
    {"IpProtocol":"tcp","FromPort":22,"ToPort":22,"IpRanges":[{"CidrIp":"0.0.0.0/0"}]},
    {"IpProtocol":"icmp","FromPort":-1,"ToPort":-1,"IpRanges":[{"CidrIp":"0.0.0.0/0"}]}
  ]'

SG_CLUSTER=$(aws ec2 create-security-group \
  --group-name "$NAME-sg-cluster" \
  --description "cluster internal" \
  --vpc-id "$VPC_ID" \
  --query 'GroupId' --output text)

aws ec2 authorize-security-group-ingress --group-id "$SG_CLUSTER" \
  --ip-permissions "[
    {\"IpProtocol\":\"-1\",\"IpRanges\":[{\"CidrIp\":\"$VPC_CIDR\"}]}
  ]"

AMI_ID=$(aws ssm get-parameter \
  --name /aws/service/canonical/ubuntu/server/22.04/stable/current/amd64/hvm/ebs-gp2/ami-id \
  --query 'Parameter.Value' --output text)
echo "Using AMI: $AMI_ID"

ENI1=$(aws ec2 create-network-interface \
  --subnet-id "$CL_SUBNET_ID" \
  --groups "$SG_CLUSTER" \
  --query 'NetworkInterface.NetworkInterfaceId' --output text)

ENI2=$(aws ec2 create-network-interface \
  --subnet-id "$CL_SUBNET_ID" \
  --groups "$SG_CLUSTER" \
  --query 'NetworkInterface.NetworkInterfaceId' --output text)

INST1=$(aws ec2 run-instances \
  --image-id "$AMI_ID" \
  --instance-type c7i.2xlarge \
  --key-name "$KEY_NAME" \
  --placement "AvailabilityZone=$AZ" \
  --network-interfaces \
    "DeviceIndex=0,SubnetId=$PUB_SUBNET_ID,Groups=$SG_PUBLIC" \
    "DeviceIndex=1,NetworkInterfaceId=$ENI1" \
  --tag-specifications "ResourceType=instance,Tags=[{Key=Name,Value=$NAME-1}]" \
  --query 'Instances[0].InstanceId' --output text)

echo "Instance1: $INST1"

INST2=$(aws ec2 run-instances \
  --image-id "$AMI_ID" \
  --instance-type c7i.2xlarge \
  --key-name "$KEY_NAME" \
  --placement "AvailabilityZone=$AZ" \
  --network-interfaces \
    "DeviceIndex=0,SubnetId=$PUB_SUBNET_ID,Groups=$SG_PUBLIC" \
    "DeviceIndex=1,NetworkInterfaceId=$ENI2" \
  --tag-specifications "ResourceType=instance,Tags=[{Key=Name,Value=$NAME-2}]" \
  --query 'Instances[0].InstanceId' --output text)

echo "Instance2: $INST2"

aws ec2 describe-instances --instance-ids "$INST1" "$INST2" \
  --query 'Reservations[].Instances[].{Name:Tags[?Key==`Name`].Value|[0],PublicIP:PublicIpAddress,PrivIPs:PrivateIpAddresses[*].PrivateIpAddress}' \
  --output table

