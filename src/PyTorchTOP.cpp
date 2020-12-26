/* Shared Use License: This file is owned by Derivative Inc. (Derivative)
* and can only be used, and/or modified for use, in conjunction with
* Derivative's TouchDesigner software, and only if you are a licensee who has
* accepted Derivative's TouchDesigner license or assignment agreement
* (which also govern the use of this file). You may share or redistribute
* a modified version of this file provided the following conditions are met:
*
* 1. The shared file or redistribution must retain the information set out
* above and this list of conditions.
* 2. Derivative's name (Derivative Inc.) or its trademarks may not be used
* to endorse or promote products derived from this file without specific
* prior written permission from Derivative.
*/

#include "PyTorchTOP.h"

#include <c10/cuda/CUDACachingAllocator.h>
#include <torch/script.h>
#include "cuda_runtime.h"

#include <assert.h>
#ifdef __APPLE__
#include <OpenGL/gl3.h>
#include <string.h>
#endif
#include <cstdio>

#include <filesystem>

// These functions are basic C function, which the DLL loader can find
// much easier than finding a C++ Class.
// The DLLEXPORT prefix is needed so the compile exports these functions from the .dll
// you are creating
extern "C"
{
DLLEXPORT
void
FillTOPPluginInfo(TOP_PluginInfo *info)
{
	// todo: justify removing this precautionary step.
	// https://github.com/pytorch/pytorch/issues/33415
	LoadLibraryA("c10_cuda.dll");
	LoadLibraryA("torch_cuda.dll");

	// This must always be set to this constant
	info->apiVersion = TOPCPlusPlusAPIVersion;

	// Change this to change the executeMode behavior of this plugin.
	info->executeMode = TOP_ExecuteMode::CUDA;

	// The opType is the unique name for this TOP. It must start with a 
	// capital A-Z character, and all the following characters must lower case
	// or numbers (a-z, 0-9)
	info->customOPInfo.opType->setString("Backgroundmatte");

	// The opLabel is the text that will show up in the OP Create Dialog
	info->customOPInfo.opLabel->setString("Background Matte TOP");

	// Will be turned into a 3 letter icon on the nodes
	info->customOPInfo.opIcon->setString("BMT");

	// Information about the author of this OP
	info->customOPInfo.authorName->setString("David Braun");
	info->customOPInfo.authorEmail->setString("github.com/DBraun");

	// This TOP requires exactly 2 inputs.
	info->customOPInfo.minInputs = 2;
	info->customOPInfo.maxInputs = 2;
}

DLLEXPORT
TOP_CPlusPlusBase*
CreateTOPInstance(const OP_NodeInfo* info, TOP_Context *context)
{
	// Return a new instance of your class every time this is called.
	// It will be called once per TOP that is using the .dll

    // Note we can't do any OpenGL work during instantiation

	return new PyTorchTOP(info, context);
}

DLLEXPORT
void
DestroyTOPInstance(TOP_CPlusPlusBase* instance, TOP_Context *context)
{
	// Delete the instance here, this will be called when
	// Touch is shutting down, when the TOP using that instance is deleted, or
	// if the TOP loads a different DLL

    // We do some OpenGL teardown on destruction, so ask the TOP_Context
    // to set up our OpenGL context

	delete (PyTorchTOP*)instance;

}

};


PyTorchTOP::PyTorchTOP(const OP_NodeInfo* info, TOP_Context *context)
: myNodeInfo(info), myExecuteCount(0)
{
	myError.str("");	
}

PyTorchTOP::~PyTorchTOP()
{
	// https://discuss.pytorch.org/t/how-to-manually-delete-free-a-tensor-in-aten/64153/2
	c10::cuda::CUDACachingAllocator::emptyCache();
}

void
PyTorchTOP::getGeneralInfo(TOP_GeneralInfo* ginfo, const OP_Inputs *inputs, void* reserved)
{
	// Setting cookEveryFrame to true causes the TOP to cook every frame even
	// if none of its inputs/parameters are changing. Set it to false if it
    // only needs to cook when inputs/parameters change.
	ginfo->cookEveryFrame = false;
}

bool
PyTorchTOP::getOutputFormat(TOP_OutputFormat* format, const OP_Inputs *inputs, void* reserved)
{

	int myOutputWidth, myOutputHeight;
	// note that myOutputNumChannels is a variable of the class.

	inputs->getParInt3("Outputresolution", myOutputWidth, myOutputHeight, myOutputNumChannels);

	format->width = myOutputWidth;
	format->height = myOutputHeight;

	format->redChannel = true;
	format->greenChannel = false;
	format->blueChannel = false;
	format->alphaChannel = false;
	if (myOutputNumChannels > 1) {
		format->greenChannel = true;
	}
	if (myOutputNumChannels > 2) {
		format->blueChannel = true;
	}
	if (myOutputNumChannels > 3) {
		format->alphaChannel = true;
	}

	format->bitsPerChannel = myBytesperoutputchannel*8;
	format->floatPrecision = myBytesperoutputchannel > 1;

	return true;
}

torch::Dtype bytesToDtype(int numBytes) {
	if (numBytes == 4) {
		return torch::kFloat32;
	}
	else if (numBytes == 2) {
		return torch::kFloat16;
	}
	else {
		return torch::kByte;
	}
}

bool
PyTorchTOP::setupLibtorch(int inputWidth, int inputHeight, int inputChannels, torch::Dtype dtype) {

	try {

		std::cout << "Trying to setup U2NetTOP. Wait for success. " << torch::cuda::is_available() << std::endl;
		LoadLibraryA("torch_cuda.dll");
		LoadLibraryA("c10_cuda.dll");

		std::cout << "    CUDA available?:   " << torch::cuda::is_available() << std::endl;
		std::cout << "    CUDNN available?:  " << torch::cuda::cudnn_is_available() << std::endl;
		std::cout << "    # visible GPU(s): " << torch::cuda::device_count() << std::endl;

		const torch::TensorOptions tensorOptions = torch::TensorOptions(myDevice = myDevice).dtype(dtype);
		myInputTensorForeground = torch::ones({ 1, inputHeight, inputWidth, inputChannels }, tensorOptions);
		myInputTensorBackground = torch::ones({ 1, inputHeight, inputWidth, inputChannels }, tensorOptions);

		std::cout << "    Creating CUDA tensor..." << std::endl;
		std::cout << "CUDA available?:   " << torch::cuda::is_available() << std::endl;
	}
	catch (std::exception& ex) {
		myError << "Unable to setup libtorch. Error: " << ex.what();
		return false;
	}

	return true;
}

bool
PyTorchTOP::checkModelFile(const char* newModelFilePath) {

	if (myEmptyString.compare(newModelFilePath) == 0) {

		myError << "Model file path is empty.";
		return false;
	}

	else if (myLoadedModelFilePath.compare(newModelFilePath) == 0) {
		// we had previously gotten to the end with this filepath, so presumably
		//   everything is still working fine.
		return true;
	}

	try {

		if (!std::filesystem::exists(newModelFilePath)) {
			myError << "Model file does not exist at path: " << newModelFilePath;
			return false;
		}

		myModule = torch::jit::load(newModelFilePath, torch::kCUDA);
		myModule.eval();
		myModule.to(torch::kCUDA);
		myMainModel = std::make_shared<WrapperModel>(myIntermediateDtype, myModule);
	}
	catch (const c10::Error & e) {

		myError << "Error loading model: " << e.msg();
		return false;
	}
	catch (...) {
		myError << "Unknown error loading the model.";
		return false;
	}

	// Save the name of the model we just successfully loaded.
	myLoadedModelFilePath = newModelFilePath;

	return true;
}

void
PyTorchTOP::setModelParameters(const OP_Inputs* inputs) {
	int Refinemode = inputs->getParInt("Refinemode");
	if (Refinemode != myRefineMode) {
		switch (Refinemode) {

		case 0:
			myModule.setattr("refine_mode", "full");
			break;
		case 1:
			myModule.setattr("refine_mode", "sampling");
			break;
		case 2:
			myModule.setattr("refine_mode", "thresholding");
			break;
		default:
			myModule.setattr("refine_mode", "full");
			break;
		}
		myRefineMode = Refinemode;
	}

	int Backbonescale = inputs->getParInt("Backbonescale");
	if (Backbonescale != myBackboneScale) {
		switch (Refinemode) {

		case 0:
			myModule.setattr("backbone_scale", 0.25);
			break;
		case 1:
			myModule.setattr("backbone_scale", 0.5);
			break;
		case 2:
			myModule.setattr("backbone_scale", 1.0);
			break;
		default:
			myModule.setattr("backbone_scale", 1.0);
			break;
		}
		myBackboneScale = Backbonescale;
	}

	int Refinesamplepixels = inputs->getParInt("Refinesamplepixels");
	if (Refinesamplepixels != myRefineSamplePixels) {
		myModule.setattr("refine_sample_pixels", myRefineSamplePixels);
	}
}

bool
PyTorchTOP::checkInputTOP(const OP_TOPInput* inputTOP, int desiredWidth, int desiredHeight) {

	if (inputTOP->width != desiredWidth || inputTOP->height != desiredHeight) {
		myError << "Input texture must be size (" << desiredWidth << "," << desiredHeight << ").";
		return false;
	}

	if (inputTOP->cudaInput == nullptr)
	{
		myError << "CUDA memory for input TOP was not mapped correctly.";
		return false;
	}

	// todo: For extra safety, you can check the pixel format.
	//if (inputTOP->pixelFormat != GL_RGBA32F) {
	//	myError << "Input texture must be 32-bit.";
	//	return;
	//}

	return true;
}


void
PyTorchTOP::execute(TOP_OutputFormatSpecs* outputFormat ,
							const OP_Inputs* inputs,
							TOP_Context* context,
							void* reserved)
{
	myExecuteCount++;

	myError.clear();
	myError.str("");

	const OP_TOPInput* foregroundInput = inputs->getInputTOP(0);
	const OP_TOPInput* backgroundInput = inputs->getInputTOP(1);

	if (!foregroundInput || !backgroundInput)
	{
		myError << "This plugin requires 2 inputs.";
		return;
	}

	int myInputWidth, myInputHeight, myInputChannels;
	inputs->getParInt3("Inputresolution", myInputWidth, myInputHeight, myInputChannels);

	int Bytesperinputchannel = inputs->getParInt("Bytesperinputchannel");
	myBytesperoutputchannel = (size_t) inputs->getParInt("Bytesperoutputchannel");
	myInputDtype = bytesToDtype(Bytesperinputchannel);
	myIntermediateDtype = bytesToDtype(inputs->getParInt("Bytespermodelinputchannel"));

	if (!checkInputTOP(foregroundInput, myInputWidth, myInputHeight)) return;
	if (!checkInputTOP(backgroundInput, myInputWidth, myInputHeight)) return;

	if (!myHasSetup) {
		myHasSetup = setupLibtorch(myInputWidth, myInputHeight, myInputChannels, myInputDtype);
		if (!myHasSetup) {
			return;
		}
	}

	if (!checkModelFile(inputs->getParFilePath("Modelfile"))) {
		return;
	}

	setModelParameters(inputs);
			
	size_t inBytes = (size_t) (myInputWidth) * (size_t) myInputHeight * (size_t)myInputChannels * (size_t) Bytesperinputchannel;
	
	cudaError_t cudaErr;
	cudaErr = cudaMemcpyFromArray(myInputTensorForeground.data_ptr(), foregroundInput->cudaInput, 0, 0, inBytes, cudaMemcpyDeviceToDevice);
	if (cudaErr != cudaSuccess) {
		myError << "Unable to CUDA copy to tensor: " << cudaErr;
		return;
	}

	cudaErr = cudaMemcpyFromArray(myInputTensorBackground.data_ptr(), backgroundInput->cudaInput, 0, 0, inBytes, cudaMemcpyDeviceToDevice);
	if (cudaErr != cudaSuccess) {
		myError << "Unable to CUDA copy to tensor: " << cudaErr;
		return;
	}

	auto start = high_resolution_clock::now();

	auto alpha = myMainModel->forward( myInputTensorForeground , myInputTensorBackground);
	
	auto stop = high_resolution_clock::now();
	myDuration = duration_cast<milliseconds>(stop - start);

	//std::cout << alpha.sizes() << std::endl;

	auto torchOutputPtr = alpha.data_ptr();

	size_t spitch = (size_t) (outputFormat->width) * myOutputNumChannels * myBytesperoutputchannel;
	size_t outPitch = (size_t) (outputFormat->width) * myOutputNumChannels * myBytesperoutputchannel;

	// http://developer.download.nvidia.com/compute/cuda/3_1/toolkit/docs/online/group__CUDART__MEMORY_g1cc6e4eb2a5e0cd2bebbc8ebb4b6c46f.html
	cudaErr = cudaMemcpy2DToArray(outputFormat->cudaOutput[0], 0, 0, torchOutputPtr, spitch, outPitch, outputFormat->height, cudaMemcpyDeviceToDevice);
	if (cudaErr != cudaSuccess) {
		myError << "Error copying tensor result back to TouchDesigner: " << cudaErr;
		return;
	}

}

int32_t
PyTorchTOP::getNumInfoCHOPChans(void* reserved)
{
	// We return the number of channel we want to output to any Info CHOP
	// connected to the TOP. In this example we are just going to send one channel.
	return 2;
}

void
PyTorchTOP::getInfoCHOPChan(int32_t index,
						OP_InfoCHOPChan* chan,
						void* reserved)
{
	// This function will be called once for each channel we said we'd want to return
	// In this example it'll only be called once.

	if (index == 0)
	{
		chan->name->setString("executeCount");
		chan->value = (float)myExecuteCount;
	}
	else if (index == 1) {
		chan->name->setString("forwardCookTime");
		chan->value = myDuration.count();
	}
}

bool		
PyTorchTOP::getInfoDATSize(OP_InfoDATSize* infoSize, void* reserved)
{
	infoSize->rows = 1;
	infoSize->cols = 2;
	// Setting this to false means we'll be assigning values to the table
	// one row at a time. True means we'll do it one column at a time.
	infoSize->byColumn = false;
	return true;
}

void
PyTorchTOP::getInfoDATEntries(int32_t index,
										int32_t nEntries,
										OP_InfoDATEntries* entries,
										void* reserved)
{
	char tempBuffer[4096];

	if (index == 0)
	{
		// Set the value for the first column
#ifdef _WIN32
		strcpy_s(tempBuffer, "executeCount");
#else // macOS
        strlcpy(tempBuffer, "executeCount", sizeof(tempBuffer));
#endif
		entries->values[0]->setString(tempBuffer);

		// Set the value for the second column
#ifdef _WIN32
		sprintf_s(tempBuffer, "%d", myExecuteCount);
#else // macOS
        snprintf(tempBuffer, sizeof(tempBuffer), "%d", myExecuteCount);
#endif
		entries->values[1]->setString(tempBuffer);
	}
}

void
PyTorchTOP::getErrorString(OP_String *error, void* reserved)
{
	error->setString(myError.str().c_str());
}

void
PyTorchTOP::setupParameters(OP_ParameterManager* manager, void* reserved)
{
	//// pulse
	//{
	//	OP_NumericParameter	np;

	//	np.name = "Reset";
	//	np.label = "Reset";
	//	
	//	OP_ParAppendResult res = manager->appendPulse(np);
	//	assert(res == OP_ParAppendResult::Success);
	//}

	{
		OP_NumericParameter np;
		np.name = "Inputresolution";
		np.defaultValues[0] = 320;
		np.defaultValues[1] = 320;
		np.defaultValues[2] = 3;

		np.label = "Input Resolution";

		OP_ParAppendResult res = manager->appendInt(np, 3);
		assert(res == OP_ParAppendResult::Success);
	}

	{
		OP_NumericParameter np;
		np.name = "Bytesperinputchannel";
		np.defaultValues[0] = 4;
		np.clampMins[0] = true;
		np.clampMaxes[0] = true;
		np.minValues[0] = 1;
		np.maxValues[0] = 4;
		np.minSliders[0] = 1;
		np.maxSliders[0] = 4;

		np.label = "Bytes Per Input Channel";

		OP_ParAppendResult res = manager->appendInt(np);
		assert(res == OP_ParAppendResult::Success);
	}

	{
		OP_NumericParameter np;
		np.name = "Bytespermodelinputchannel";
		np.defaultValues[0] = 4;
		np.clampMins[0] = true;
		np.clampMaxes[0] = true;
		np.minValues[0] = 1;
		np.maxValues[0] = 4;
		np.minSliders[0] = 1;
		np.maxSliders[0] = 4;

		np.label = "Bytes Per Model Input Channel";

		OP_ParAppendResult res = manager->appendInt(np);
		assert(res == OP_ParAppendResult::Success);
	}

	{
		OP_NumericParameter np;
		np.name = "Outputresolution";
		np.defaultValues[0] = 320;
		np.defaultValues[1] = 320;
		np.defaultValues[2] = 1;

		np.label = "Output Resolution";

		OP_ParAppendResult res = manager->appendInt(np, 3);
		assert(res == OP_ParAppendResult::Success);
	}

	{
		OP_NumericParameter np;
		np.name = "Bytesperoutputchannel";
		np.defaultValues[0] = 4;
		np.clampMins[0] = true;
		np.clampMaxes[0] = true;
		np.minValues[0] = 1;
		np.maxValues[0] = 4;
		np.minSliders[0] = 1;
		np.maxSliders[0] = 4;

		np.label = "Bytes Per Output Channel";

		OP_ParAppendResult res = manager->appendInt(np);
		assert(res == OP_ParAppendResult::Success);
	}

	{
		OP_StringParameter sp;

		sp.name = "Modelfile";
		sp.label = "Model File";
		sp.defaultValue = "";
		OP_ParAppendResult res = manager->appendFile(sp);
		assert(res == OP_ParAppendResult::Success);
	}

	{
		OP_StringParameter	sp;
		sp.page = "Matte";
		sp.name = "Refinemode";
		sp.label = "Refine Mode";

		sp.defaultValue = "full";

		const char* names[] = { "Full", "Sampling", "Thresholding" };
		const char* labels[] = { "full", "sampling", "thresholding" };

		OP_ParAppendResult res = manager->appendMenu(sp, 3, names, labels);
		assert(res == OP_ParAppendResult::Success);
	}

	{
		OP_StringParameter	sp;
		sp.page = "Matte";
		sp.name = "Backbonescale";
		sp.label = "Backbone Scale";

		sp.defaultValue = "quarter";

		const char* names[] = { "Quarter", "Half", "One" };
		const char* labels[] = { "Quarter", "Half", "One" };

		OP_ParAppendResult res = manager->appendMenu(sp, 3, names, labels);
		assert(res == OP_ParAppendResult::Success);
	}

	{
		OP_NumericParameter np;
		np.page = "Matte";
		np.name = "Refinesamplepixels";
		np.defaultValues[0] = 80000;
		np.minSliders[0] = 0;
		np.maxSliders[0] = 500000;
		np.clampMins[0] = true;
		np.minValues[0] = 0;

		np.label = "Refine Sample Pixels";

		OP_ParAppendResult res = manager->appendInt(np, 1);
		assert(res == OP_ParAppendResult::Success);
	}

}

void
PyTorchTOP::pulsePressed(const char* name, void* reserved)
{
	//if (!strcmp(name, "Reset"))
	//{
	//	// Do something to reset here
	//}
}