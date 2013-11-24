///////////////////////////////////////////////////////////////////////////////
///
///	\file    Grid.cpp
///	\author  Paul Ullrich
///	\version February 25, 2013
///
///	<remarks>
///		Copyright 2000-2010 Paul Ullrich
///
///		This file is distributed as part of the Tempest source code package.
///		Permission is granted to use, copy, modify and distribute this
///		source code and its documentation under the terms of the GNU General
///		Public License.  This software is provided "as is" without express
///		or implied warranty.
///	</remarks>

#include "Grid.h"
#include "Model.h"
#include "TestCase.h"
#include "ConsolidationStatus.h"

#include "Exception.h"

#include <cfloat>
#include <cmath>

#include <netcdfcpp.h>
#include "mpi.h"

///////////////////////////////////////////////////////////////////////////////

Grid::Grid(
	const Model & model,
	int nABaseResolution,
	int nBBaseResolution,
	int nRefinementRatio,
	int nRElements
) :
	m_fInitialized(false),
	m_iGridStamp(0),
	m_model(model),
	m_nABaseResolution(nABaseResolution),
	m_nBBaseResolution(nBBaseResolution),
	m_nRefinementRatio(nRefinementRatio),
	m_dReferenceLength(1.0),
	m_nRElements(nRElements),
	m_dZtop(1.0)
{
}

///////////////////////////////////////////////////////////////////////////////

Grid::~Grid() {
	for (int n = 0; n < m_vecGridPatches.size(); n++) {
		delete m_vecGridPatches[n];
	}
}

///////////////////////////////////////////////////////////////////////////////

void Grid::InitializeVerticalCoordinate(
	const GridSpacing & gridspacing
) {
	// Initialize location and index for each variable
	m_vecVarLocation.resize(m_model.GetEquationSet().GetComponents());

	// If dimensionality is 2, then initialize a dummy REta
	if (m_model.GetEquationSet().GetDimensionality() == 2) {

		// Resize arrays of REta coordinates
		m_dREtaLevels.Initialize(1);
		m_dREtaInterfaces.Initialize(2);

		// Uniform grid spacing
		m_dREtaInterfaces[0] = 0.0;
		m_dREtaInterfaces[1] = 1.0;
		m_dREtaLevels[0] = 0.5;

#pragma message "Do not hardcode this information"
		// Everything on model levels
		m_vecVarLocation[0] = DataLocation_Node;
		m_vecVarLocation[1] = DataLocation_Node;
		m_vecVarLocation[2] = DataLocation_Node;

	// If dimensionality is 3 then initialize normally
	} else if (m_model.GetEquationSet().GetDimensionality() == 3) {

		// Check for agreement with grid spacing
		if (!gridspacing.DoesNodeCountAgree(m_nRElements)) {
			_EXCEPTIONT("Invalid node count for given vertical GridSpacing.");
		}

		// Zero point of GridSpacing object
		double dZeroCoord = gridspacing.GetZeroCoord();

		// Resize arrays of REta coordinates
		m_dREtaLevels.Initialize(m_nRElements);
		m_dREtaInterfaces.Initialize(m_nRElements+1);

		// Uniform grid spacing
		for (int k = 0; k <= m_nRElements; k++) {
			m_dREtaInterfaces[k] = gridspacing.GetEdge(k);
		}
		for (int k = 0; k < m_nRElements; k++) {
			m_dREtaLevels[k] = gridspacing.GetNode(k);
		}

		// Location of variables
		m_vecVarLocation[0] = DataLocation_Node;
		m_vecVarLocation[1] = DataLocation_Node;
		m_vecVarLocation[2] = DataLocation_REdge;
		m_vecVarLocation[3] = DataLocation_REdge;
		m_vecVarLocation[4] = DataLocation_Node;

	} else {
		_EXCEPTIONT("Invalid dimensionality");
	}

	// Convert node locations to indices in local arrays
	m_vecVarsAtLocation.resize((int)DataLocation_Count);
	for (int l = 0; l < (int)DataLocation_Count; l++) {
		m_vecVarsAtLocation[l] = 0;
	}

	m_vecVarIndex.resize(m_model.GetEquationSet().GetComponents());
	for (int c = 0; c < m_model.GetEquationSet().GetComponents(); c++) {
		if (m_vecVarLocation[c] == DataLocation_Node) {
			m_vecVarIndex[c] = m_vecVarsAtLocation[(int)DataLocation_Node]++;
		} else if (m_vecVarLocation[c] == DataLocation_AEdge) {
			m_vecVarIndex[c] = m_vecVarsAtLocation[(int)DataLocation_AEdge]++;
		} else if (m_vecVarLocation[c] == DataLocation_BEdge) {
			m_vecVarIndex[c] = m_vecVarsAtLocation[(int)DataLocation_BEdge]++;
		} else if (m_vecVarLocation[c] == DataLocation_REdge) {
			m_vecVarIndex[c] = m_vecVarsAtLocation[(int)DataLocation_REdge]++;
		} else {
			_EXCEPTIONT("Invalid variable location");
		}
	}
}

///////////////////////////////////////////////////////////////////////////////

void Grid::EvaluateTestCase(
	const TestCase & test,
	const Time & time,
	int iDataIndex
) {
	// Store the model cap
	m_dZtop = test.GetZtop();
    //std::cout << "\n" << m_dZtop << "\n";

	// Store the reference state flag
	m_fHasReferenceState = test.HasReferenceState();

	// Evaluate the pointwise values of the test
	for (int n = 0; n < m_vecActiveGridPatches.size(); n++) {
		m_vecActiveGridPatches[n]->
			EvaluateTestCase(test, time, iDataIndex);
	}
}

///////////////////////////////////////////////////////////////////////////////

void Grid::EvaluateGeometricTerms() {
	for (int n = 0; n < m_vecActiveGridPatches.size(); n++) {
		m_vecActiveGridPatches[n]->EvaluateGeometricTerms();
	}
}

///////////////////////////////////////////////////////////////////////////////

void Grid::Checksum(
	DataType eDataType,
	DataVector<double> & dChecksums,
	int iDataIndex,
	ChecksumType eChecksumType
) const {

	// Identify root process
	int nRank;
	MPI_Comm_rank(MPI_COMM_WORLD, &nRank);

	// Initialize the local checksum array from DataType
	DataVector<double> dChecksumsLocal;
	if (eDataType == DataType_State) {
		dChecksumsLocal.Initialize(m_model.GetEquationSet().GetComponents());

	} else if (eDataType == DataType_Tracers) { 
		dChecksumsLocal.Initialize(m_model.GetEquationSet().GetTracers());
		if (m_model.GetEquationSet().GetTracers() == 0) {
			return;
		}

	} else {
		_EXCEPTIONT("Invalid DataType");
	}

	// Loop over all patches and calculate local checksums
	for (int n = 0; n < m_vecActiveGridPatches.size(); n++) {
		m_vecActiveGridPatches[n]->Checksum(
			eDataType, dChecksumsLocal, iDataIndex, eChecksumType);
	}

	// Initialize global checksums array at root
	if (nRank == 0) {
		dChecksums.Initialize(dChecksumsLocal.GetRows());
	}

	// Compute sum over all processors and send to root node
	MPI_Op nMPIOperator;
	if (eChecksumType == ChecksumType_Linf) {
		nMPIOperator = MPI_MAX;
	} else {
		nMPIOperator = MPI_SUM;
	}

	MPI_Reduce(
		&(dChecksumsLocal[0]),
		&(dChecksums[0]),
		dChecksumsLocal.GetRows(),
		MPI_DOUBLE,
		nMPIOperator,
		0,
		MPI_COMM_WORLD);

	// Take the square root for the L2 norm sum
	if (nRank == 0) {
		if (eChecksumType == ChecksumType_L2) {
			for (int c = 0; c < dChecksums.GetRows(); c++) {
				dChecksums[c] = sqrt(dChecksums[c]);
			}
		}
	}
}

///////////////////////////////////////////////////////////////////////////////

void Grid::Exchange(
	DataType eDataType,
	int iDataIndex
) {
	// Verify all processors are prepared to exchange
	MPI_Barrier(MPI_COMM_WORLD);

	// Set up asynchronous recvs
	for (int n = 0; n < m_vecActiveGridPatches.size(); n++) {
		m_vecActiveGridPatches[n]->PrepareExchange();
	}

	// Send data
	for (int n = 0; n < m_vecActiveGridPatches.size(); n++) {
		m_vecActiveGridPatches[n]->Send(eDataType, iDataIndex);
	}

	// Receive data
	for (int n = 0; n < m_vecActiveGridPatches.size(); n++) {
		m_vecActiveGridPatches[n]->Receive(eDataType, iDataIndex);
	}
}

///////////////////////////////////////////////////////////////////////////////

int Grid::GetLargestGridPatchNodes() const {

	// Most nodes per patch
	int nMaxNodes = 0;

	// Loop over all patches
	for (int n = 0; n < m_vecGridPatches.size(); n++) {
		int nPatchNodes =
			m_vecGridPatches[n]->GetPatchBox().GetTotalNodes();

		if (nPatchNodes > nMaxNodes) {
			nMaxNodes = nPatchNodes;
		}
	}

	return nMaxNodes;
}

///////////////////////////////////////////////////////////////////////////////

int Grid::GetLongestActivePatchPerimeter() const {

	// Longest perimeter
	int nLongestPerimeter = 0;

	// Loop over all active patches
	for (int n = 0; n < m_vecActiveGridPatches.size(); n++) {
		int nPerimeter =
			m_vecActiveGridPatches[n]->GetPatchBox().GetInteriorPerimeter();

		if (nPerimeter > nLongestPerimeter) {
			nLongestPerimeter = nPerimeter;
		}
	}

	return nLongestPerimeter;
}

///////////////////////////////////////////////////////////////////////////////

int Grid::GetTotalNodeCount() const {

	// Total number of nodes over all patches of grid
	int nTotalNodes = 0;

	// Loop over all patches and obtain total node count
	for (int n = 0; n < m_vecGridPatches.size(); n++) {
		nTotalNodes += m_vecGridPatches[n]->GetTotalNodeCount();
	}

	return nTotalNodes;
}

///////////////////////////////////////////////////////////////////////////////

int Grid::GetMaximumDegreesOfFreedom() const {

	// Most nodes per patch
	int nMaxDOFs = 0;

	// Loop over all patches and obtain max DOFs from state and tracer data
	for (int n = 0; n < m_vecGridPatches.size(); n++) {
		int nStateDOFs =
			m_model.GetEquationSet().GetComponents()
			* m_nRElements
			* m_vecGridPatches[n]->GetPatchBox().GetTotalNodes();

		int nTracersDOFs =
			m_model.GetEquationSet().GetTracers()
			* m_nRElements
			* m_vecGridPatches[n]->GetPatchBox().GetTotalNodes();

		if (nTracersDOFs > nMaxDOFs) {
			nMaxDOFs = nTracersDOFs;
		}
		if (nStateDOFs > nMaxDOFs) {
			nMaxDOFs = nStateDOFs;
		}
	}

	return nMaxDOFs;
}

///////////////////////////////////////////////////////////////////////////////

void Grid::ConsolidateDataAtRoot(
	ConsolidationStatus & status,
	DataVector<double> & dataRecvBuffer,
	int & nRecvCount,
	int & ixRecvPatch,
	DataType & eRecvDataType
) const {

	// Get process id
	int nRank;
	MPI_Comm_rank(MPI_COMM_WORLD, &nRank);

	// Non-root processes should not call this function
	if (nRank != 0) {
		_EXCEPTIONT("Non-root process calling ConsolidateDataAtRoot");
	}

	// Check if done
	if (status.Done()) {
		_EXCEPTIONT("Attempting to consolidate data after completion");
	}

	// Receive a consolidation message
	MPI_Status mpistatus;

	MPI_Recv(
		&(dataRecvBuffer[0]),
		dataRecvBuffer.GetRows(),
		MPI_DOUBLE,
		MPI_ANY_SOURCE,
		MPI_ANY_TAG,
		MPI_COMM_WORLD,
		&mpistatus);

	// Process tag for DataType and global patch index
	ConsolidationStatus::ParseTag(
		mpistatus.MPI_TAG, ixRecvPatch, eRecvDataType);

	if ((ixRecvPatch < 0) || (ixRecvPatch >= m_vecGridPatches.size())) {
		_EXCEPTIONT("Panel tag index out of range");
	}

	status.SetReceiveStatus(ixRecvPatch, eRecvDataType);

	// Verify consistency of patch information
	GridPatch * pPatch = m_vecGridPatches[ixRecvPatch];

	MPI_Get_count(&mpistatus, MPI_DOUBLE, &nRecvCount);

	if (eRecvDataType == DataType_State) {
		int nExpectedRecvCount =
			m_model.GetEquationSet().GetComponents()
			* m_nRElements
			* pPatch->GetPatchBox().GetTotalNodes();

		if (nExpectedRecvCount != nRecvCount) {
			_EXCEPTIONT("State dimension mismatch");
		}

	} else if (eRecvDataType == DataType_Tracers) {
		int nExpectedRecvCount =
			m_model.GetEquationSet().GetTracers()
			* m_nRElements
			* pPatch->GetPatchBox().GetTotalNodes();

		if (nExpectedRecvCount != nRecvCount) {
			_EXCEPTIONT("Tracers dimension mismatch");
		}

	} else if (eRecvDataType == DataType_Jacobian) {
		int nExpectedRecvCount =
			m_nRElements
			* pPatch->GetPatchBox().GetTotalNodes();

		int nDiff =
			GetCumulativePatch3DNodeIndex(ixRecvPatch+1)
			- GetCumulativePatch3DNodeIndex(ixRecvPatch);

		if (nExpectedRecvCount != nRecvCount) {
			_EXCEPTIONT("Jacobian dimension mismatch");
		}
	}
#pragma message "Perform check for other data types"
}

///////////////////////////////////////////////////////////////////////////////

void Grid::ConsolidateDataToRoot(
	ConsolidationStatus & status
) const {

	// Get process id
	int nRank;
	MPI_Comm_rank(MPI_COMM_WORLD, &nRank);

	// If no tracers, Tracer data should not be consolidated
	if ((status.Contains(DataType_Tracers)) &&
		(m_model.GetEquationSet().GetTracers() == 0)
	) {
		_EXCEPTIONT("Attempting to consolidate empty tracer data");
	}

	// Loop over all patches
	for (int n = 0; n < m_vecActiveGridPatches.size(); n++) {
		const GridPatch * pPatch = m_vecActiveGridPatches[n];

		// Data
		const GridData4D & dataState    = pPatch->GetDataState(0);
		const GridData4D & dataTracers  = pPatch->GetDataTracers(0);

		const DataMatrix3D<double> & dataJacobian   = pPatch->GetJacobian();
		const DataMatrix<double>   & dataTopography = pPatch->GetTopography();
		const DataMatrix<double>   & dataLongitude  = pPatch->GetLongitude();
		const DataMatrix<double>   & dataLatitude   = pPatch->GetLatitude();
		const DataMatrix3D<double> & dataZLevels    = pPatch->GetZLevels();

		// Send state data to root process
		if (status.Contains(DataType_State)) {
			MPI_Isend(
				(void*)(dataState[0][0][0]),
				dataState.GetTotalElements(),
				MPI_DOUBLE,
				0,
				ConsolidationStatus::GenerateTag(
					pPatch->GetPatchIndex(), DataType_State),
				MPI_COMM_WORLD,
				status.GetNextSendRequest());
		}

		// Send tracer data to root process
		if (status.Contains(DataType_Tracers)) {
			MPI_Isend(
				(void*)(dataTracers[0][0][0]),
				dataTracers.GetTotalElements(),
				MPI_DOUBLE,
				0,
				ConsolidationStatus::GenerateTag(
					pPatch->GetPatchIndex(), DataType_Tracers),
				MPI_COMM_WORLD,
				status.GetNextSendRequest());
		}

		// Send Jacobian data to root process
		if (status.Contains(DataType_Jacobian)) {
			MPI_Isend(
				(void*)(dataJacobian[0][0]),
				dataJacobian.GetTotalElements(),
				MPI_DOUBLE,
				0,
				ConsolidationStatus::GenerateTag(
					pPatch->GetPatchIndex(), DataType_Jacobian),
				MPI_COMM_WORLD,
				status.GetNextSendRequest());
		}

		// Send topography data to root process
		if (status.Contains(DataType_Topography)) {
			MPI_Isend(
				(void*)(dataTopography[0]),
				dataTopography.GetTotalElements(),
				MPI_DOUBLE,
				0,
				ConsolidationStatus::GenerateTag(
					pPatch->GetPatchIndex(), DataType_Topography),
				MPI_COMM_WORLD,
				status.GetNextSendRequest());
		}

		// Send longitude data to root process
		if (status.Contains(DataType_Longitude)) {
			MPI_Isend(
				(void*)(dataLongitude[0]),
				dataLongitude.GetTotalElements(),
				MPI_DOUBLE,
				0,
				ConsolidationStatus::GenerateTag(
					pPatch->GetPatchIndex(), DataType_Longitude),
				MPI_COMM_WORLD,
				status.GetNextSendRequest());
		}

		// Send latitude data to root process
		if (status.Contains(DataType_Latitude)) {
			MPI_Isend(
				(void*)(dataLatitude[0]),
				dataLatitude.GetTotalElements(),
				MPI_DOUBLE,
				0,
				ConsolidationStatus::GenerateTag(
					pPatch->GetPatchIndex(), DataType_Latitude),
				MPI_COMM_WORLD,
				status.GetNextSendRequest());
		}

		// Send z-coordinate data to root process
		if (status.Contains(DataType_Z)) {
			MPI_Isend(
				(void*)(dataZLevels[0][0]),
				dataZLevels.GetTotalElements(),
				MPI_DOUBLE,
				0,
				ConsolidationStatus::GenerateTag(
					pPatch->GetPatchIndex(), DataType_Z),
				MPI_COMM_WORLD,
				status.GetNextSendRequest());
		}
	}
}

///////////////////////////////////////////////////////////////////////////////

void Grid::ComputeVorticityDivergence(
	int iDataIndex
) {
	// Loop over all grid patches
	for (int n = 0; n < m_vecActiveGridPatches.size(); n++) {
		m_vecActiveGridPatches[n]->ComputeVorticityDivergence(iDataIndex);
	}
}

///////////////////////////////////////////////////////////////////////////////

void Grid::InterpolateNodeToREdge(
	int iVar,
	int iDataIndex
) {
	// Loop over all grid patches
	for (int n = 0; n < m_vecActiveGridPatches.size(); n++) {
		m_vecActiveGridPatches[n]->InterpolateNodeToREdge(iVar, iDataIndex);
	}
}

///////////////////////////////////////////////////////////////////////////////

void Grid::InterpolateREdgeToNode(
	int iVar,
	int iDataIndex
) {
	// Loop over all grid patches
	for (int n = 0; n < m_vecActiveGridPatches.size(); n++) {
		m_vecActiveGridPatches[n]->InterpolateREdgeToNode(iVar, iDataIndex);
	}
}

///////////////////////////////////////////////////////////////////////////////

void Grid::ReduceInterpolate(
	const DataVector<double> & dAlpha,
	const DataVector<double> & dBeta,
	const DataVector<int> & iPatch,
	DataType eDataType,
	DataMatrix3D<double> & dInterpData,
	bool fIncludeReferenceState,
	bool fConvertToPrimitive
) const {
	// Check interpolation data array size
	if ((dAlpha.GetRows() != dBeta.GetRows()) ||
		(dAlpha.GetRows() != iPatch.GetRows())
	) {
		_EXCEPTIONT("Inconsistency in vector lengths.");
	}

	if ((eDataType == DataType_Tracers) &&
		(m_model.GetEquationSet().GetTracers() == 0)
	) {
		_EXCEPTIONT("Unable to Interpolate with no tracers.");
	}
	
	// Check interpolation data array size
	if ((eDataType == DataType_State) &&
		(dInterpData.GetRows() != m_model.GetEquationSet().GetComponents())
	) {
		_EXCEPTIONT("InterpData dimension mismatch (0)");
	}

	if ((eDataType == DataType_Tracers) &&
		(dInterpData.GetRows() != m_model.GetEquationSet().GetTracers())
	) {
		_EXCEPTIONT("InterpData dimension mismatch (0)");
	}

	if ((eDataType == DataType_Vorticity) &&
		(dInterpData.GetRows() != 1)
	) {
		_EXCEPTIONT("InterpData dimension mismatch (0)");
	}

	if ((eDataType == DataType_Divergence) &&
		(dInterpData.GetRows() != 1)
	) {
		_EXCEPTIONT("InterpData dimension mismatch (0)");
	}

	if (dInterpData.GetColumns() != GetRElements()) {
		_EXCEPTIONT("InterpData dimension mismatch (1)");
	}

	if (dInterpData.GetSubColumns() != dAlpha.GetRows()) {
		_EXCEPTIONT("InterpData dimension mismatch (2)");
	}

	// Zero the interpolated data
	dInterpData.Zero();

	// Interpolate state data
	for (int n = 0; n < m_vecActiveGridPatches.size(); n++) {
		m_vecActiveGridPatches[n]->InterpolateData(
			dAlpha, dBeta, iPatch,
			eDataType,
			dInterpData,
			fIncludeReferenceState,
			fConvertToPrimitive);
	}

	// Perform an Reduce operation to combine all data
	int nRank;
	MPI_Comm_rank(MPI_COMM_WORLD, &nRank);

	if (nRank == 0) {
		MPI_Reduce(
			MPI_IN_PLACE,
			&(dInterpData[0][0][0]),
			dInterpData.GetRows()
				* dInterpData.GetColumns()
				* dInterpData.GetSubColumns(),
			MPI_DOUBLE,
			MPI_SUM,
			0,
			MPI_COMM_WORLD);

	} else {
		MPI_Reduce(
			&(dInterpData[0][0][0]),
			NULL,
			dInterpData.GetRows()
				* dInterpData.GetColumns()
				* dInterpData.GetSubColumns(),
			MPI_DOUBLE,
			MPI_SUM,
			0,
			MPI_COMM_WORLD);
	}
}

///////////////////////////////////////////////////////////////////////////////

void Grid::ConvertReferenceToPatchCoord(
	const DataVector<double> & dXReference,
	const DataVector<double> & dYReference,
	DataVector<double> & dAlpha,
	DataVector<double> & dBeta,
	DataVector<int> & iPatch
) const {
	_EXCEPTIONT("Unimplemented.");
}

///////////////////////////////////////////////////////////////////////////////

GridPatch * Grid::AddPatch(
	GridPatch * pPatch
) {
	int ixNextPatch = m_vecGridPatches.size();

	// Add the patch to the vector of patches
	m_vecGridPatches.push_back(pPatch);

	// Set the patch index
	pPatch->m_ixPatch = ixNextPatch;

	// Update the cumulative 2D index
	if (ixNextPatch == 0) {
		m_vecCumulativePatch2DNodeIndex.push_back(0);
	}

	m_vecCumulativePatch2DNodeIndex.push_back(
		m_vecCumulativePatch2DNodeIndex[ixNextPatch]
		+ pPatch->GetTotalNodeCount());

	return pPatch;
}

///////////////////////////////////////////////////////////////////////////////

void Grid::ToFile(
	NcFile & ncfile
) {

	// Create patch index dimension
	NcDim * dimPatchIndex =
		ncfile.add_dim("patch_index", GetPatchCount());

	// Get the length of each dimension array
	int iANodeIndex = 0;
	int iBNodeIndex = 0;
	int iAEdgeIndex = 0;
	int iBEdgeIndex = 0;

	int nANodeCount = 0;
	int nBNodeCount = 0;
	int nAEdgeCount = 0;
	int nBEdgeCount = 0;

	for (int n = 0; n < GetPatchCount(); n++) {
		const PatchBox & box = GetPatch(n)->GetPatchBox();

		nANodeCount += box.GetANodes().GetRows();
		nBNodeCount += box.GetBNodes().GetRows();
		nAEdgeCount += box.GetAEdges().GetRows();
		nBEdgeCount += box.GetBEdges().GetRows();
	}

	NcDim * dimANodeCount =
		ncfile.add_dim("alpha_node_index", nANodeCount);
	NcDim * dimBNodeCount =
		ncfile.add_dim("beta_node_index", nBNodeCount);
	NcDim * dimAEdgeCount =
		ncfile.add_dim("alpha_edge_index", nAEdgeCount);
	NcDim * dimBEdgeCount =
		ncfile.add_dim("beta_edge_index", nBEdgeCount);

	NcVar * varANodeCoord =
		ncfile.add_var(
			"alpha_node_coord", ncDouble, dimANodeCount);
	NcVar * varBNodeCoord =
		ncfile.add_var(
			"beta_node_coord", ncDouble, dimBNodeCount);
	NcVar * varAEdgeCoord =
		ncfile.add_var(
			"alpha_edge_coord", ncDouble, dimAEdgeCount);
	NcVar * varBEdgeCoord =
		ncfile.add_var(
			"beta_edge_coord", ncDouble, dimBEdgeCount);

	// Output global resolution
	int iGridInfo[4];
	iGridInfo[0] = m_iGridStamp;
	iGridInfo[1] = m_nABaseResolution;
	iGridInfo[2] = m_nBBaseResolution;
	iGridInfo[3] = m_nRefinementRatio;

	NcDim * dimGridInfoCount =
		ncfile.add_dim("grid_info_count", 4);

	NcVar * varGridInfo =
		ncfile.add_var("grid_info", ncInt, dimGridInfoCount);

	varGridInfo->put(iGridInfo, 4);

	// Output PatchBox for each patch
	NcDim * dimPatchInfoCount =
		ncfile.add_dim("patch_info_count", 7);

	NcVar * varPatchInfo =
		ncfile.add_var("patch_info", ncInt, dimPatchIndex, dimPatchInfoCount);

	for (int n = 0; n < GetPatchCount(); n++) {
		int iPatchInfo[7];

		const PatchBox & box = GetPatch(n)->GetPatchBox();

		iPatchInfo[0] = box.GetPanel();
		iPatchInfo[1] = box.GetRefinementLevel();
		iPatchInfo[2] = box.GetHaloElements();
		iPatchInfo[3] = box.GetAGlobalInteriorBegin();
		iPatchInfo[4] = box.GetAGlobalInteriorEnd();
		iPatchInfo[5] = box.GetBGlobalInteriorBegin();
		iPatchInfo[6] = box.GetBGlobalInteriorEnd();

		varPatchInfo->set_cur(n, 0);
		varPatchInfo->put(iPatchInfo, 1, 7);

		varANodeCoord->set_cur(iANodeIndex);
		varBNodeCoord->set_cur(iBNodeIndex);
		varAEdgeCoord->set_cur(iAEdgeIndex);
		varBEdgeCoord->set_cur(iBEdgeIndex);

		varANodeCoord->put(box.GetANodes(), box.GetANodes().GetRows());
		varBNodeCoord->put(box.GetBNodes(), box.GetBNodes().GetRows());
		varAEdgeCoord->put(box.GetAEdges(), box.GetAEdges().GetRows());
		varBEdgeCoord->put(box.GetBEdges(), box.GetBEdges().GetRows());

		iANodeIndex += box.GetANodes().GetRows();
		iBNodeIndex += box.GetBNodes().GetRows();
		iAEdgeIndex += box.GetAEdges().GetRows();
		iBEdgeIndex += box.GetBEdges().GetRows();
	}
}

///////////////////////////////////////////////////////////////////////////////

void Grid::FromFile(
	const std::string & strGridFile
) {
	// Check for existing patches
	if (GetPatchCount() != 0) {
		_EXCEPTIONT("Trying to load over non-empty grid");
	}

	// Open the NetCDF file
	NcFile ncfile(strGridFile.c_str(), NcFile::ReadOnly);

	// Load in grid info in alpha and beta direction
	int iGridInfo[4];

	NcVar * varGridInfo = ncfile.get_var("grid_info");

	varGridInfo->get(iGridInfo, 4);

	m_iGridStamp = iGridInfo[0];
	m_nABaseResolution = iGridInfo[1];
	m_nBBaseResolution = iGridInfo[2];
	m_nRefinementRatio = iGridInfo[3];

	// Coordinate arrays
	int iANodeIndex = 0;
	int iBNodeIndex = 0;
	int iAEdgeIndex = 0;
	int iBEdgeIndex = 0;

	DataVector<double> dANodes;
	DataVector<double> dBNodes;
	DataVector<double> dAEdges;
	DataVector<double> dBEdges;

	// Load in all PatchBoxes
	NcVar * varPatchInfo = ncfile.get_var("patch_info");
	if (varPatchInfo == NULL) {
		_EXCEPTIONT("Invalid GridFile; variable patch_info required");
	}

	NcVar * varANodeCoord = ncfile.get_var("alpha_node_coord");
	if (varANodeCoord == NULL) {
		_EXCEPTIONT("Invalid GridFile; variable alpha_node_coord required");
	}

	NcVar * varBNodeCoord = ncfile.get_var("beta_node_coord");
	if (varBNodeCoord == NULL) {
		_EXCEPTIONT("Invalid GridFile; variable beta_node_coord required");
	}

	NcVar * varAEdgeCoord = ncfile.get_var("alpha_edge_coord");
	if (varAEdgeCoord == NULL) {
		_EXCEPTIONT("Invalid GridFile; variable alpha_edge_coord required");
	}

	NcVar * varBEdgeCoord = ncfile.get_var("beta_edge_coord");
	if (varBEdgeCoord == NULL) {
		_EXCEPTIONT("Invalid GridFile; variable beta_edge_coord required");
	}

	NcDim * dimPatchInfoCount = varPatchInfo->get_dim(0);
	int nPatches = static_cast<int>(dimPatchInfoCount->size());

	for (int ix = 0; ix < nPatches; ix++) {
		int iPatchInfo[7];
		varPatchInfo->set_cur(ix, 0);
		varPatchInfo->get(iPatchInfo, 1, 7);

		int nANodes = iPatchInfo[4] - iPatchInfo[3] + 2 * iPatchInfo[2];
		int nBNodes = iPatchInfo[6] - iPatchInfo[5] + 2 * iPatchInfo[2];

		dANodes.Initialize(nANodes);
		dBNodes.Initialize(nBNodes);
		dAEdges.Initialize(nANodes+1);
		dBEdges.Initialize(nBNodes+1);

		varANodeCoord->set_cur(iANodeIndex);
		varBNodeCoord->set_cur(iBNodeIndex);
		varAEdgeCoord->set_cur(iAEdgeIndex);
		varBEdgeCoord->set_cur(iBEdgeIndex);

		varANodeCoord->get(dANodes, nANodes);
		varBNodeCoord->get(dBNodes, nBNodes);
		varAEdgeCoord->get(dAEdges, nANodes+1);
		varBEdgeCoord->get(dBEdges, nBNodes+1);

		PatchBox box(
			iPatchInfo[0],
			iPatchInfo[1],
			iPatchInfo[2],
			iPatchInfo[3],
			iPatchInfo[4],
			iPatchInfo[5],
			iPatchInfo[6],
			dANodes,
			dBNodes,
			dAEdges,
			dBEdges);

		AddPatch(ix, box);

		iANodeIndex += box.GetANodes().GetRows();
		iBNodeIndex += box.GetBNodes().GetRows();
		iAEdgeIndex += box.GetAEdges().GetRows();
		iBEdgeIndex += box.GetBEdges().GetRows();
	}
}

///////////////////////////////////////////////////////////////////////////////

void Grid::DistributePatches() {

	// Number of processors
	int nSize;
	MPI_Comm_size(MPI_COMM_WORLD, &nSize);

	// Current processor
	int nRank;
	MPI_Comm_rank(MPI_COMM_WORLD, &nRank);

	// Loop over all patches and initialize data
	for (int n = 0; n < m_vecGridPatches.size(); n++) {
		int nPatchProcessor = n % nSize;
		if (nPatchProcessor == nRank) {
			m_vecGridPatches[n]->InitializeDataLocal();
			m_vecActiveGridPatches.push_back(m_vecGridPatches[n]);
		} else {
			m_vecGridPatches[n]->InitializeDataRemote(nPatchProcessor);
		}
	}
}

///////////////////////////////////////////////////////////////////////////////

void Grid::InitializeConnectivity() {

	// Vector of nodal points around element
	DataVector<int> vecIxA;
	DataVector<int> vecIxB;
	DataVector<int> vecPanel;
	DataVector<int> vecPatch;

	// Determine longest perimeter
	int nLongestActivePerimeter = GetLongestActivePatchPerimeter() + 4;
	vecIxA.Initialize(nLongestActivePerimeter);
	vecIxB.Initialize(nLongestActivePerimeter);
	vecPanel.Initialize(nLongestActivePerimeter);
	vecPatch.Initialize(nLongestActivePerimeter);

	// Loop over all active patches
	for (int n = 0; n < m_vecActiveGridPatches.size(); n++) {

		GridPatch * pPatch = m_vecActiveGridPatches[n];

		const PatchBox & box = pPatch->GetPatchBox();

		int ix = 0;

		// Bottom-left corner
		vecIxA[ix] = box.GetAGlobalInteriorBegin()-1;
		vecIxB[ix] = box.GetBGlobalInteriorBegin()-1;
		vecPanel[ix] = box.GetPanel();
		ix++;

		// Bottom edge
		for (int i = box.GetAGlobalInteriorBegin();
		         i < box.GetAGlobalInteriorEnd(); i++
		) {
			vecIxA[ix] = i;
			vecIxB[ix] = box.GetBGlobalInteriorBegin()-1;
			vecPanel[ix] = box.GetPanel();
			ix++;
		}

		// Bottom-right corner
		vecIxA[ix] = box.GetAGlobalInteriorEnd();
		vecIxB[ix] = box.GetBGlobalInteriorBegin()-1;
		vecPanel[ix] = box.GetPanel();
		ix++;

		// Right edge
		for (int j = box.GetBGlobalInteriorBegin();
		         j < box.GetBGlobalInteriorEnd(); j++
		) {
			vecIxA[ix] = box.GetAGlobalInteriorEnd();
			vecIxB[ix] = j;
			vecPanel[ix] = box.GetPanel();
			ix++;
		}

		// Top-right corner
		vecIxA[ix] = box.GetAGlobalInteriorEnd();
		vecIxB[ix] = box.GetBGlobalInteriorEnd();
		vecPanel[ix] = box.GetPanel();
		ix++;

		// Top edge
		for (int i = box.GetAGlobalInteriorEnd()-1;
		         i >= box.GetAGlobalInteriorBegin(); i--
		) {
			vecIxA[ix] = i;
			vecIxB[ix] = box.GetBGlobalInteriorEnd();
			vecPanel[ix] = box.GetPanel();
			ix++;
		}

		// Top-left corner
		vecIxA[ix] = box.GetAGlobalInteriorBegin()-1;
		vecIxB[ix] = box.GetBGlobalInteriorEnd();
		vecPanel[ix] = box.GetPanel();
		ix++;

		// Left edge
		for (int j = box.GetBGlobalInteriorEnd()-1;
		         j >= box.GetBGlobalInteriorBegin(); j--
		) {
			vecIxA[ix] = box.GetAGlobalInteriorBegin()-1;
			vecIxB[ix] = j;
			vecPanel[ix] = box.GetPanel();
			ix++;
		}

		// Get neighboring patches at each halo node
		GetPatchFromCoordinateIndex(
			box.GetRefinementLevel(),
			vecIxA,
			vecIxB,
			vecPanel,
			vecPatch,
			ix);

		// Verify index length
		if (ix != box.GetInteriorPerimeter() + 4) {
			_EXCEPTIONT("Index mismatch");
		}

		// Reset index
		ix = 0;

		// Add connectivity to bottom-left corner
		if (vecPatch[ix] != GridPatch::InvalidIndex) {
			Connectivity::ExteriorConnect(
				pPatch,
				Direction_BottomLeft,
				m_vecGridPatches[vecPatch[ix]]);
		}

		ix++;

		// Add connectivity to bottom edge: Look for segments along each
		// edge that connect to distinct patches and construct corresponding
		// ExteriorNeighbors.
		{
			int ixFirstBegin = box.GetAInteriorBegin();
			int iCurrentPatch = vecPatch[ix];

			for (int i = ixFirstBegin; i <= box.GetAInteriorEnd(); i++) {
				if ((i == box.GetAInteriorEnd()) ||
					(vecPatch[ix] != iCurrentPatch)
				) {
					const GridPatch * pPatchBottom
						= m_vecGridPatches[iCurrentPatch];

					Connectivity::ExteriorConnect(
						pPatch,
						Direction_Bottom,
						pPatchBottom,
						ixFirstBegin,
						i);

					if (i != box.GetAInteriorEnd()) {
						Connectivity::ExteriorConnect(
							pPatch,
							Direction_BottomLeft,
							pPatchBottom,
							i,
							box.GetBInteriorBegin());

						ixFirstBegin = i;
						iCurrentPatch = vecPatch[ix];
						pPatchBottom = m_vecGridPatches[iCurrentPatch];

						Connectivity::ExteriorConnect(
							pPatch,
							Direction_BottomRight,
							pPatchBottom,
							i - 1,
							box.GetBInteriorBegin());
					}
				}
				if (i != box.GetAInteriorEnd()) {
					ix++;
				}
			}
		}

		// Add connectivity to bottom-right corner
		if (vecPatch[ix] != GridPatch::InvalidIndex) {
			Connectivity::ExteriorConnect(
				pPatch,
				Direction_BottomRight,
				m_vecGridPatches[vecPatch[ix]]);
		}

		ix++;

		// Add connectivity to right edge
		{
			int ixFirstBegin = box.GetBInteriorBegin();
			int iCurrentPatch = vecPatch[ix];

			for (int j = ixFirstBegin; j <= box.GetBInteriorEnd(); j++) {
				if ((j == box.GetBInteriorEnd()) ||
					(vecPatch[ix] != iCurrentPatch)
				) {
					const GridPatch * pPatchRight
						= m_vecGridPatches[iCurrentPatch];

					Connectivity::ExteriorConnect(
						pPatch,
						Direction_Right,
						pPatchRight,
						ixFirstBegin,
						j);

					if (j != box.GetBInteriorEnd()) {
						Connectivity::ExteriorConnect(
							pPatch,
							Direction_BottomRight,
							pPatchRight,
							box.GetAInteriorEnd()-1,
							j);

						ixFirstBegin = j;
						iCurrentPatch = vecPatch[ix];
						pPatchRight = m_vecGridPatches[iCurrentPatch];

						Connectivity::ExteriorConnect(
							pPatch,
							Direction_TopRight,
							pPatchRight,
							box.GetAInteriorEnd()-1,
							j - 1);
					}
				}
				if (j != box.GetBInteriorEnd()) {
					ix++;
				}
			}
		}

		// Add connectivity to top-right corner
		if (vecPatch[ix] != GridPatch::InvalidIndex) {
			Connectivity::ExteriorConnect(
				pPatch,
				Direction_TopRight,
				m_vecGridPatches[vecPatch[ix]]);
		}

		ix++;

		// Add connectivity to top edge
		{
			int ixFirstEnd = box.GetAInteriorEnd();
			int iCurrentPatch = vecPatch[ix];

			for (int i = ixFirstEnd-1; i >= box.GetAInteriorBegin()-1; i--) {
				if ((i == box.GetAInteriorBegin()-1) ||
					(vecPatch[ix] != iCurrentPatch)
				) {
					const GridPatch * pPatchTop =
						m_vecGridPatches[iCurrentPatch];

					Connectivity::ExteriorConnect(
						pPatch,
						Direction_Top,
						pPatchTop,
						i + 1,
						ixFirstEnd);

					if (i != box.GetAInteriorBegin()-1) {
						Connectivity::ExteriorConnect(
							pPatch,
							Direction_TopRight,
							pPatchTop,
							i,
							box.GetBInteriorEnd()-1);

						ixFirstEnd = i + 1;
						iCurrentPatch = vecPatch[ix];
						pPatchTop = m_vecGridPatches[iCurrentPatch];

						Connectivity::ExteriorConnect(
							pPatch,
							Direction_TopLeft,
							pPatchTop,
							i + 1,
							box.GetBInteriorEnd()-1);
					}
				}
				if (i != box.GetAInteriorBegin()-1) {
					ix++;
				}
			}
		}

		// Add connectivity to top-left corner
		if (vecPatch[ix] != GridPatch::InvalidIndex) {
			Connectivity::ExteriorConnect(
				pPatch,
				Direction_TopLeft,
				m_vecGridPatches[vecPatch[ix]]);
		}

		ix++;

		// Add connectivity to top edge
		{
			int ixFirstEnd = box.GetBInteriorEnd();
			int iCurrentPatch = vecPatch[ix];

			for (int j = ixFirstEnd-1; j >= box.GetBInteriorBegin()-1; j--) {
				if ((j == box.GetBInteriorBegin()-1) ||
					(vecPatch[ix] != iCurrentPatch)
				) {
					const GridPatch * pPatchLeft =
						m_vecGridPatches[iCurrentPatch];

					Connectivity::ExteriorConnect(
						pPatch,
						Direction_Left,
						pPatchLeft,
						j + 1,
						ixFirstEnd);

					if (j != box.GetBInteriorBegin()-1) {
						Connectivity::ExteriorConnect(
							pPatch,
							Direction_TopLeft,
							pPatchLeft,
							box.GetAInteriorBegin(),
							j);

						ixFirstEnd = j + 1;
						iCurrentPatch = vecPatch[ix];
						pPatchLeft = m_vecGridPatches[iCurrentPatch];

						Connectivity::ExteriorConnect(
							pPatch,
							Direction_BottomLeft,
							pPatchLeft,
							box.GetAInteriorBegin(),
							j + 1);
					}
				}
				if (j != box.GetBInteriorBegin()-1) {
					ix++;
				}
			}
		}

		// Verify index length
		if (ix != box.GetInteriorPerimeter() + 4) {
			_EXCEPTIONT("Index mismatch");
		}
	}
}

///////////////////////////////////////////////////////////////////////////////

void Grid::CopyData(
	int ixSource,
	int ixDest,
	DataType eDataType
) {
	// Loop over all grid patches
	for (int n = 0; n < m_vecActiveGridPatches.size(); n++) {
		m_vecActiveGridPatches[n]->
			CopyData(ixSource, ixDest, eDataType);
	}
}

///////////////////////////////////////////////////////////////////////////////

void Grid::LinearCombineData(
	const DataVector<double> & dCoeff,
	int ixDest,
	DataType eDataType
) {
	// Loop over all grid patches
	for (int n = 0; n < m_vecActiveGridPatches.size(); n++) {
		m_vecActiveGridPatches[n]->
			LinearCombineData(dCoeff, ixDest, eDataType);
	}
}

///////////////////////////////////////////////////////////////////////////////

void Grid::ZeroData(
	int ixData,
	DataType eDataType
) {
	// Loop over all grid patches
	for (int n = 0; n < m_vecActiveGridPatches.size(); n++) {
		m_vecActiveGridPatches[n]->ZeroData(ixData, eDataType);
	}
}

///////////////////////////////////////////////////////////////////////////////

void Grid::AddReferenceState(
	int ix
) {
	// Loop over all grid patches
	for (int n = 0; n < m_vecActiveGridPatches.size(); n++) {
		m_vecActiveGridPatches[n]->AddReferenceState(ix);
	}
}

///////////////////////////////////////////////////////////////////////////////

