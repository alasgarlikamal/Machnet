# used the terminal provided by the google cloud console to run these commands

PROJECT_ID="$(gcloud config get-value project)"
REGION="us-west1"
ZONE="us-west1-b"

VPC_PUBLIC="vpc-public"
SUBNET_PUBLIC="subnet-public"

VPC_CLUSTER="vpc-cluster"
SUBNET_CLUSTER="subnet-cluster"

VM1_NAME="latency-node-1"
VM2_NAME="latency-node-2"

gcloud compute networks create "$VPC_PUBLIC" \
  --subnet-mode=custom

gcloud compute networks subnets create "$SUBNET_PUBLIC" \
  --network="$VPC_PUBLIC" \
  --region="$REGION" \
  --range=10.10.0.0/24

gcloud compute networks create "$VPC_CLUSTER" \
  --subnet-mode=custom

gcloud compute networks subnets create "$SUBNET_CLUSTER" \
  --network="$VPC_CLUSTER" \
  --region="$REGION" \
  --range=10.20.0.0/24

gcloud compute firewall-rules create allow-ssh-icmp-public \
  --network="$VPC_PUBLIC" \
  --allow=tcp:22,icmp \
  --direction=INGRESS \
  --source-ranges=0.0.0.0/0

gcloud compute firewall-rules create allow-internal-cluster \
  --network="$VPC_CLUSTER" \
  --allow=tcp,udp,icmp \
  --direction=INGRESS \
  --source-ranges=10.20.0.0/24

gcloud compute instances create "$VM1_NAME" \
  --zone="$ZONE" \
  --machine-type="c3-standard-4" \
  --network-interface=network="$VPC_PUBLIC",subnet="$SUBNET_PUBLIC",network-tier=PREMIUM,stack-type=IPV4_ONLY \
  --network-interface=network="$VPC_CLUSTER",subnet="$SUBNET_CLUSTER",network-tier=PREMIUM,stack-type=IPV4_ONLY,nic-type=gvnic \
  --image-family="ubuntu-2204-lts" \
  --image-project="ubuntu-os-cloud" \
  --boot-disk-size=20GB \
  --boot-disk-type=pd-ssd \
  --tags="latency-test"

gcloud compute instances create "$VM2_NAME" \
  --zone="$ZONE" \
  --machine-type="c3-standard-4" \
  --network-interface=network="$VPC_PUBLIC",subnet="$SUBNET_PUBLIC",network-tier=PREMIUM,stack-type=IPV4_ONLY \
  --network-interface=network="$VPC_CLUSTER",subnet="$SUBNET_CLUSTER",network-tier=PREMIUM,stack-type=IPV4_ONLY,nic-type=gvnic \
  --image-family="ubuntu-2204-lts" \
  --image-project="ubuntu-os-cloud" \
  --boot-disk-size=20GB \
  --boot-disk-type=pd-ssd \
  --tags="latency-test"

