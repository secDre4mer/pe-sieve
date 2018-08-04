#include "artefact_scanner.h"

#include "../utils/workingset_enum.h"

#include "peconv.h"
#include "peconv/fix_imports.h"

#define PE_NOT_FOUND 0

bool is_valid_section(BYTE *loadedData, size_t loadedSize, BYTE *hdr_ptr, DWORD charact)
{
	PIMAGE_SECTION_HEADER hdr_candidate = (PIMAGE_SECTION_HEADER) hdr_ptr;
	if (!peconv::validate_ptr(loadedData, loadedSize, hdr_candidate, sizeof(IMAGE_SECTION_HEADER))) {
		// probably buffer finished
		return false;
	}
	if (hdr_candidate->PointerToRelocations != 0
		|| hdr_candidate->NumberOfRelocations != 0
		|| hdr_candidate->PointerToLinenumbers != 0)
	{
		//values that should be NULL are not
		return false;
	}
	if (charact != 0 && (hdr_candidate->Characteristics & charact) == 0) {
		// required characteristics not found
		//std::cout << "The section " << hdr_candidate->Name << " NOT  valid, charact:" << std::hex << hdr_candidate->Characteristics << std::endl;
		return false;
	}
	//std::cout << "The section " << hdr_candidate->Name << " is valid!" << std::endl;
	return true;
}

size_t count_section_hdrs(BYTE *loadedData, size_t loadedSize, IMAGE_SECTION_HEADER *hdr_ptr)
{
	size_t counter = 0;
	IMAGE_SECTION_HEADER* curr_sec = hdr_ptr;
	do {
		if (!is_valid_section(loadedData, loadedSize, (BYTE*)curr_sec, IMAGE_SCN_MEM_READ)) {
			break;
		}
		curr_sec++;
		counter++;
	} while (true);

	return counter;
}

//calculate image size basing on the sizes of sections
DWORD ArtefactScanner::calcImageSize(MemPageData &memPage, IMAGE_SECTION_HEADER *hdr_ptr)
{
	DWORD max_addr = 0;
	IMAGE_SECTION_HEADER* curr_sec = hdr_ptr;
	DWORD sec_rva = 0;
	size_t max_sec_size = 0;
	do {
		if (!is_valid_section(memPage.loadedData, memPage.loadedSize, (BYTE*)curr_sec, IMAGE_SCN_MEM_READ)) {
			break;
		}
		sec_rva = curr_sec->VirtualAddress;
#ifdef _DEBUG
		DWORD sec_size = curr_sec->Misc.VirtualSize;
		ULONGLONG sec_va = (ULONGLONG)memPage.region_start + sec_rva;
		size_t real_sec_size = fetch_region_size(processHandle, (PBYTE)sec_va);
		if (sec_size > real_sec_size) {
			std::cout << "[WARNING] Corrupt section size: " << std::hex
				<< sec_size << " vs real: " << real_sec_size << std::endl;
		}
#endif
		max_addr = (sec_rva > max_addr) ? sec_rva : max_addr;
		curr_sec++;

	} while (true);

	ULONGLONG last_sec_va = (ULONGLONG)memPage.region_start + max_addr;
	size_t last_sec_size = fetch_region_size(processHandle, (PBYTE)last_sec_va);
	size_t total_size = max_addr + last_sec_size;
#ifdef _DEBUG
	std::cout << "Total Size:" << std::hex << total_size << std::endl;
#endif
	return total_size;
}

IMAGE_SECTION_HEADER* get_first_section(BYTE *loadedData, size_t loadedSize, IMAGE_SECTION_HEADER *hdr_ptr)
{
	IMAGE_SECTION_HEADER* prev_sec = hdr_ptr;
	do {
		if (!is_valid_section(loadedData, loadedSize, (BYTE*) prev_sec, IMAGE_SCN_MEM_READ)) {
			break;
		}
		hdr_ptr = prev_sec;
		prev_sec--;
	} while (true);

	return hdr_ptr;
}

BYTE* ArtefactScanner::findSecByPatterns(MemPageData &memPage)
{
	if (memPage.loadedData == nullptr) {
		if (!memPage.loadRemote()) return nullptr;
		if (memPage.loadedData == nullptr) return nullptr;
	}
	//find sections table
	char sec_name[] = ".text";
	BYTE *hdr_ptr = find_pattern(memPage.loadedData, memPage.loadedSize, (BYTE*)sec_name, strlen(sec_name));
	if (hdr_ptr) {
		return hdr_ptr;
	}
	// try another pattern
	BYTE sec_ending[] = {
		0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00,
		0x20, 0x00, 0x00, 0x60
	};
	const size_t sec_ending_size = sizeof(sec_ending);
	hdr_ptr = find_pattern(memPage.loadedData, memPage.loadedSize, sec_ending, sec_ending_size);
	if (!hdr_ptr) {
		return nullptr;
	}
	size_t offset_to_bgn = sizeof(IMAGE_SECTION_HEADER) - sec_ending_size;
	hdr_ptr -= offset_to_bgn;
	if (!peconv::validate_ptr(memPage.loadedData, memPage.loadedSize, hdr_ptr, sizeof(IMAGE_SECTION_HEADER))) {
		return nullptr;
	}
	return hdr_ptr;
}

IMAGE_SECTION_HEADER* ArtefactScanner::findSectionsHdr(MemPageData &memPage)
{
	BYTE *hdr_ptr = findSecByPatterns(memPage);
	if (!hdr_ptr) {
		return nullptr;
	}
	DWORD charact = IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_EXECUTE;
	if (!is_valid_section(memPage.loadedData, memPage.loadedSize, hdr_ptr, charact)) {
		return nullptr;
	}
	// is it really the first section?
	IMAGE_SECTION_HEADER *first_sec = get_first_section(memPage.loadedData, memPage.loadedSize, (IMAGE_SECTION_HEADER*) hdr_ptr);
	return (IMAGE_SECTION_HEADER*)first_sec;
}

bool is_valid_file_hdr(BYTE *loadedData, size_t loadedSize, BYTE *hdr_ptr, DWORD charact)
{
	IMAGE_FILE_HEADER* hdr_candidate = (IMAGE_FILE_HEADER*)hdr_ptr;
	if (!peconv::validate_ptr(loadedData, loadedSize, hdr_candidate, sizeof(IMAGE_FILE_HEADER))) {
		// probably buffer finished
		return false;
	}
	if (hdr_candidate->NumberOfSections > 100) {
		return false;
	}
	if (hdr_candidate->NumberOfSymbols != 0 || hdr_candidate->PointerToSymbolTable != 0) {
		return false;
	}
	//sanity checks of machine and optional header size:
	size_t opt_hdr_size = 0;
	if (hdr_candidate->Machine == IMAGE_FILE_MACHINE_I386) {
		opt_hdr_size = sizeof(IMAGE_OPTIONAL_HEADER32);
	}
	else if (hdr_candidate->Machine == IMAGE_FILE_MACHINE_AMD64) {
		opt_hdr_size = sizeof(IMAGE_OPTIONAL_HEADER64);
	}
	else {
		// wrong machine ID
		return false;
	}

	if (hdr_candidate->SizeOfOptionalHeader < opt_hdr_size) {
		return false;
	}
	if (hdr_candidate->SizeOfOptionalHeader > PAGE_SIZE) {
		return false;
	}
	if (!peconv::validate_ptr(loadedData, loadedSize, hdr_candidate, 
		sizeof(IMAGE_FILE_HEADER) + hdr_candidate->SizeOfOptionalHeader))
	{
		return false;
	}
	//check characteristics:
	if (charact != 0 && (hdr_candidate->Characteristics & charact) == 0) {
		return false;
	}
	return true;
}

BYTE* ArtefactScanner::findNtFileHdr(BYTE* loadedData, size_t loadedSize)
{
	if (loadedData == nullptr) {
		return nullptr;
	}
	typedef enum {
		ARCH_32B = 0, 
		ARCH_64B = 1, 
		ARCHS_COUNT
	} t_archs;

	WORD archs[ARCHS_COUNT] = { 0 };
	archs[ARCH_32B] = IMAGE_FILE_MACHINE_I386;
	archs[ARCH_64B] = IMAGE_FILE_MACHINE_AMD64;

	BYTE *arch_ptr = nullptr;
	size_t my_arch = 0;
	for (my_arch = ARCH_32B; my_arch < ARCHS_COUNT; my_arch++) {
		arch_ptr = find_pattern(loadedData, loadedSize, (BYTE*)&archs[my_arch], sizeof(WORD));
		if (arch_ptr) {
			break;
		}
	}
	if (!arch_ptr) {
		return nullptr;
	}
	DWORD charact = IMAGE_FILE_EXECUTABLE_IMAGE;
	if (my_arch == ARCH_32B) {
		charact |= IMAGE_FILE_32BIT_MACHINE;
	}
	else {
		charact |= IMAGE_FILE_LARGE_ADDRESS_AWARE;
	}
	if (!is_valid_file_hdr(loadedData, loadedSize, arch_ptr, charact)) {
		return nullptr;
	}
	return arch_ptr;
}

PeArtefacts* ArtefactScanner::findArtefacts(MemPageData &memPage)
{
	IMAGE_SECTION_HEADER* sec_hdr = findSectionsHdr(memPage);
	if (!sec_hdr) {
		return nullptr;
	}
	PeArtefacts *peArt = new PeArtefacts();
	peArt->regionStart =  memPage.region_start;
	peArt->peBaseOffset = 0;

	peArt->secHdrsOffset = (ULONGLONG)sec_hdr - (ULONGLONG)memPage.loadedData;
	for (ULONGLONG offset = peArt->secHdrsOffset; offset > PAGE_SIZE; offset-=PAGE_SIZE) {
		peArt->peBaseOffset += PAGE_SIZE;
	}
	peArt->secCount = count_section_hdrs(memPage.loadedData, memPage.loadedSize, sec_hdr);
	peArt->calculatedImgSize = calcImageSize(memPage, sec_hdr);
	return peArt;
}

PeArtefacts* ArtefactScanner::findInPrevPages(ULONGLONG addr_start, ULONGLONG addr_stop)
{
	deletePrevPage();
	PeArtefacts* peArt = nullptr;
	ULONGLONG next_addr = addr_stop - PAGE_SIZE;
	do {
		if (next_addr < addr_start) {
			break;
		}
		this->prevMemPage = new MemPageData(this->processHandle, next_addr);
		peArt = findArtefacts(*prevMemPage);
		if (peArt) {
			break;
		}
		next_addr -= (this->prevMemPage->region_start - PAGE_SIZE);
		deletePrevPage();
	} while (true);

	return peArt;
}

ArtefactScanReport* ArtefactScanner::scanRemote()
{
	deletePrevPage();

	bool is_damaged_pe = false;
	// it may still contain a damaged PE header...
	ULONGLONG region_start = memPage.region_start;
	MemPageData *artPagePtr = &memPage;

	PeArtefacts *peArt = findArtefacts(memPage);
	if (!peArt  && (region_start > memPage.alloc_base)) {
		peArt = findInPrevPages(memPage.alloc_base, memPage.region_start);
		if (prevMemPage) {
			artPagePtr = prevMemPage;
			region_start = prevMemPage->region_start;
		}
	}
	if (!peArt) {
		//no artefacts found
		return nullptr;
	}

	BYTE* nt_file_hdr = findNtFileHdr(artPagePtr->loadedData, size_t(peArt->secHdrsOffset));
	if (nt_file_hdr) {
		peArt->ntFileHdrsOffset = (ULONGLONG)nt_file_hdr - (ULONGLONG)artPagePtr->loadedData;
	}

	const size_t region_size = size_t(memPage.region_end - region_start);
	ArtefactScanReport *my_report = new ArtefactScanReport(processHandle, (HMODULE)region_start, region_size, SCAN_SUSPICIOUS, *peArt);
	my_report->is_manually_loaded = !memPage.is_listed_module;
	my_report->protection = memPage.protection;

	if (peArt->calculatedImgSize > region_size) {
		my_report->moduleSize = peArt->calculatedImgSize;
	}
	delete peArt;
	return my_report;
}
